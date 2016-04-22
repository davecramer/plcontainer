#include <assert.h>

#include <errno.h>
#include <netinet/ip.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>

#include <R.h>
#include <Rversion.h>
#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>
#include <Rdefines.h>

#if (R_VERSION >= 132352) /* R_VERSION >= 2.5.0 */
#define R_PARSEVECTOR(a_, b_, c_)               R_ParseVector(a_, b_, (ParseStatus *) c_, R_NilValue)
#else /* R_VERSION < 2.5.0 */
#define R_PARSEVECTOR(a_, b_, c_)               R_ParseVector(a_, b_, (ParseStatus *) c_)
#endif /* R_VERSION >= 2.5.0 */

/* R's definition conflicts with the ones defined by postgres */
#ifdef WARNING
#undef WARNING

#define WARNING		19			/* Warnings.  NOTICE is for expected messages
								 * like implicit sequence creation by SERIAL.
								 * WARNING is for unexpected messages. */
#endif
#ifdef ERROR
#undef ERROR
#define ERROR		20			/* user error - abort transaction; return to
								 * known state */
#endif

#include "common/comm_channel.h"
#include "common/comm_utils.h"
#include "common/comm_connectivity.h"
#include "common/comm_server.h"
#include "rcall.h"
#include "rconversions.h"

void raise_execution_error (plcConn *conn, const char *format, ...);
//static SEXP coerce_to_char(SEXP rval);
SEXP convert_args(callreq req);
static SEXP arguments_to_r (plcConn *conn, plcRFunction *r_func);
static int process_call_results(plcConn *conn, SEXP retval, plcRFunction *r_func);

static void
pg_get_one_r(char *value,  plcDatatype column_type, SEXP *obj, int elnum);
static void
pg_get_null( plcDatatype column_type, SEXP *obj, int elnum);

SEXP get_r_vector(plcDatatype type_id, int numels);
int get_entry_length(plcDatatype type);

static char * create_r_func(callreq req);

#define OPTIONS_NULL_CMD    "options(error = expression(NULL))"

/* install the error handler to call our throw_r_error */
#define THROWRERROR_CMD \
            "pg.throwrerror <-function(msg) " \
            "{" \
            "  msglen <- nchar(msg);" \
            "  if (substr(msg, msglen, msglen + 1) == \"\\n\")" \
            "    msg <- substr(msg, 1, msglen - 1);" \
            "  .C(\"throw_r_error\", as.character(msg));" \
            "}"
#define OPTIONS_THROWRERROR_CMD \
            "options(error = expression(pg.throwrerror(geterrmessage())))"

/* install the notice handler to call our throw_r_notice */
#define THROWNOTICE_CMD \
            "pg.thrownotice <-function(msg) " \
            "{.C(\"throw_pg_notice\", as.character(msg))}"
#define THROWERROR_CMD \
            "pg.throwerror <-function(msg) " \
            "{stop(msg, call. = FALSE)}"
#define OPTIONS_THROWWARN_CMD \
            "options(warning.expression = expression(pg.thrownotice(last.warning)))"

#define QUOTE_LITERAL_CMD \
            "pg.quoteliteral <-function(sql) " \
            "{.Call(\"plr_quote_literal\", sql)}"
#define QUOTE_IDENT_CMD \
            "pg.quoteident <-function(sql) " \
            "{.Call(\"plr_quote_ident\", sql)}"
#define SPI_EXEC_CMD \
            "pg.spi.exec <-function(sql) {.Call(\"plr_SPI_exec\", sql)}"

#define SPI_DBGETQUERY_CMD \
            "dbGetQuery <-function(sql) {\n" \
            "data <- pg.spi.exec(sql)\n" \
            "return(data)\n" \
            "}"

int R_SignalHandlers = 1;  /* Exposed in R_interface.h */

static void load_r_cmd(const char *cmd);
static char * get_load_self_ref_cmd(const char *libstr);

// Initialization of R module
void r_init( );


/*
  based on examples from:
  1. https://github.com/parkerabercrombie/call-r-from-c/blob/master/r_test.c
  2. https://github.com/wch/r-source/tree/trunk/tests/Embedding
  3. http://pabercrombie.com/wordpress/2014/05/how-to-call-an-r-function-from-c/

  Other resources:
  - https://cran.r-project.org/doc/manuals/r-release/R-exts.html
  - http://adv-r.had.co.nz/C-interface.html
 */

static void send_error(plcConn* conn, char *msg);
//static char * create_r_func(callreq req);

///static char *create_r_func(callreq req);
static SEXP parse_r_code(const char *code, plcConn* conn, int *errorOccurred);

//static plcIterator *matrix_iterator(SEXP mtx, plcDatatype type);
//static void matrix_iterator_free(plcIterator *iter);

/*
 * set by hook throw_r_error
 */
char *last_R_error_msg,
     *last_R_notice;

extern SEXP plr_SPI_execp(const char * sql);



plcConn* plcconn;


void r_init( ) {
    char   *argv[] = {"client", "--slave", "--vanilla"};
    char   *    buf;

    /*
     * Stop R using its own signal handlers Otherwise, R will prompt the user for what to do and
         will hang in the container
    */
    R_SignalHandlers = 0;

    if( !Rf_initEmbeddedR(sizeof(argv) / sizeof(*argv), argv) ){
        //TODO: return an error
        ;
    }



    /*
     * temporarily turn off R error reporting -- it will be turned back on
     * once the custom R error handler is installed from the plr library
     */
    load_r_cmd(OPTIONS_NULL_CMD);

    /* next load the plr library into R */
    load_r_cmd(buf=get_load_self_ref_cmd("librcall.so"));
    pfree(buf);

    load_r_cmd(THROWRERROR_CMD);
    load_r_cmd(OPTIONS_THROWRERROR_CMD);
    load_r_cmd(THROWNOTICE_CMD);
    load_r_cmd(THROWERROR_CMD);
    load_r_cmd(OPTIONS_THROWWARN_CMD);
    load_r_cmd(QUOTE_LITERAL_CMD);
    load_r_cmd(QUOTE_IDENT_CMD);
    load_r_cmd(SPI_EXEC_CMD);
    load_r_cmd(SPI_DBGETQUERY_CMD);
}

static  char *get_load_self_ref_cmd(const char *libstr)
{
    char   *buf =  (char *) pmalloc(strlen(libstr) + 12 + 1);;

    sprintf(buf, "dyn.load(\"%s\")", libstr);
    return buf;
}

static void
load_r_cmd(const char *cmd)
{
    SEXP        cmdSexp,
                cmdexpr;
    int            i,
                status=0;


    PROTECT(cmdSexp = NEW_CHARACTER(1));
    SET_STRING_ELT(cmdSexp, 0, COPY_TO_USER_STRING(cmd));
    PROTECT(cmdexpr = R_PARSEVECTOR(cmdSexp, -1, &status));
    if (status != PARSE_OK) {
        UNPROTECT(2);
        goto error;
    }

    /* Loop is needed here as EXPSEXP may be of length > 1 */
    for(i = 0; i < length(cmdexpr); i++)
    {
        R_tryEval(VECTOR_ELT(cmdexpr, i), R_GlobalEnv, &status);
        if(status != 0)
        {
            goto error;
        }
    }

    UNPROTECT(2);
    return;

error:
    // TODO send error back to client
    printf("Error loading %s \n ",cmd);
    return;

}

void handle_call(callreq req, plcConn* conn) {
    SEXP             r,
                     strres,
                     call,
                     rargs,
                     obj,
                     args;

    int              i,
                     errorOccurred;

    char            *func,
                    *errmsg;

    //plcontainer_result res;

    /*
     * Keep our connection for future calls from R back to us.
    */
    plcconn = conn;

    /* wrap the input in a function and evaluate the result */

    func = create_r_func(req);

    plcRFunction *r_func = plc_R_init_function(req);
    PROTECT(r = parse_r_code(func, conn, &errorOccurred));

    pfree(func);

    if (errorOccurred) {
        //TODO send real error message
        /* run_r_code will send an error back */
        UNPROTECT(1); //r
        return;
    }

    if(req->nargs > 0)
    {
        rargs = arguments_to_r(conn, r_func);	//			convert_args(req);
        PROTECT(obj = args = allocList(req->nargs));

        for (i = 0; i < req->nargs; i++)
        {
            SETCAR(obj, VECTOR_ELT(rargs, i));
            obj = CDR(obj);
        }
        UNPROTECT(1);
        PROTECT(call = lcons(r, args));
    }
    else
    {
        PROTECT(call = allocVector(LANGSXP,1));
        SETCAR(call, r);
    }

    strres = R_tryEval(call, R_GlobalEnv, &errorOccurred);
    UNPROTECT(1); //call


    if (errorOccurred) {
        UNPROTECT(1); //r
        //TODO send real error message
        if (last_R_error_msg){
            errmsg = strdup(last_R_error_msg);
        }else{
            errmsg = strdup("Error executing\n");
            errmsg = realloc(errmsg, strlen(errmsg)+strlen(req->proc.src));
            errmsg = strcat(errmsg, req->proc.src);
        }
        send_error(conn, errmsg);
        free(errmsg);
        return;
    }

    process_call_results(conn, strres, r_func);


    plc_r_free_function(r_func);

    UNPROTECT(1);

    return;
}



static void send_error(plcConn* conn, char *msg) {
    /* an exception was thrown */
    error_message err;
    err             = pmalloc(sizeof(*err));
    err->msgtype    = MT_EXCEPTION;
    err->message    = msg;
    err->stacktrace = "";

    /* send the result back */
    plcontainer_channel_send(conn, (message)err);

    /* free the objects */
    free(err);
}

static SEXP parse_r_code(const char *code,  plcConn* conn, int *errorOccurred) {
    /* int hadError; */
    ParseStatus status;
    char *      errmsg;
    SEXP        tmp,
                rbody,
                fun;

    PROTECT(rbody = mkString(code));
    /*
      limit the number of expressions to be parsed to 2:
        - the definition of the function, i.e. f <- function() {...}
        - the call to the function f()

      kind of useful to prevent injection, but pointless since we are
      running in a container. I think -1 is equivalent to no limit.
    */
    PROTECT(tmp = R_ParseVector(rbody, -1, &status, R_NilValue));

    if (tmp != R_NilValue){
        PROTECT(fun = VECTOR_ELT(tmp, 0));
    }else{
        PROTECT(fun = R_NilValue);
    }

    if (status != PARSE_OK) {
        UNPROTECT(3);
        if (last_R_error_msg != NULL){
            errmsg  = strdup(last_R_error_msg);
        }else{
            errmsg =  strdup("Parse Error\n");
            errmsg =  realloc(errmsg, strlen(errmsg)+strlen(code));
            errmsg =  strcat(errmsg, code);
        }
        goto error;
    }

    UNPROTECT(3);
    *errorOccurred=0;
    return fun;

error:
    /*
     * set the global error flag
     */
    *errorOccurred=1;
    send_error(conn, errmsg);
    free(errmsg);
    return NULL;
}

static char * create_r_func(callreq req) {
    int    plen;
    char * mrc;
    size_t mlen = 0;

    int i;

    // calculate space required for args
    for (i=0;i<req->nargs;i++){
        // +4 for , and space
        mlen += strlen(req->args[i].name) + 4;
    }
    /*
     * room for function source and function call
     */
    mlen += strlen(req->proc.src) + strlen(req->proc.name) + 40;

    mrc  = pmalloc(mlen);
    plen = snprintf(mrc,mlen,"%s <- function(",req->proc.name);


    for (i=0;i<req->nargs;i++){

        strcat( mrc,req->args[i].name);

        /* add a comma if not the last arg */
        if ( i < (req->nargs-1) ){
            strcat(mrc,", ") ;
            plen += 2;
        }

        /* keep track of where we are copying */
        plen+=strlen(req->args[i].name);
    }

    /* finish the function definition from where we left off */
    plen = snprintf(mrc+plen, mlen, ") {%s}", req->proc.src);
    assert(plen >= 0 && ((size_t)plen) < mlen);
    return mrc;
}




SEXP get_r_array(plcArray *plcArray)
{
	SEXP   vec;
	int *dims,ndim;

	int nr =1,
		nc=1,
		nz=1,
		i,j,k;

	ndim = plcArray->meta->ndims;
	dims = plcArray->meta->dims;

	if (ndim == 1)
	{
		nr = dims[0];
	}

	else if (ndim == 2)
	{
		nr = dims[0];
		nc = dims[1];
	}
	else if (ndim == 3)
	{
		nr = dims[0];
		nc = dims[1];
		nz = dims[2];
	}

	PROTECT(vec = get_r_vector(plcArray->meta->type, plcArray->meta->size));
	int isNull=0;
	int elem_idx=0;
	int elem_len =0;
	for (i = 0; i < nr; i++)
	{
		for (j = 0; j < nc; j++)
		{
			for (k = 0; k < nz; k++)
			{
				//int	idx = (k * nr * nc) + (j * nr) + i;

				isNull = plcArray->nulls[elem_idx];
				elem_len = get_entry_length(plcArray->meta->type);

				if (!isNull){
					pg_get_one_r(plcArray->data+elem_idx * elem_len , plcArray->meta->type, &vec, elem_idx);
				}
				else{
					pg_get_null( plcArray->meta->type, &vec, elem_idx );
				}

				elem_idx++;
			}
		}
	}
	UNPROTECT(1);
	return vec;

}
static int process_call_results(plcConn *conn, SEXP retval, plcRFunction *r_func) {
    plcontainer_result res;

    /* allocate a result */
    res          = malloc(sizeof(str_plcontainer_result));
    res->msgtype = MT_RESULT;
    res->names   = malloc(1 * sizeof(char*));
    res->types   = malloc(1 * sizeof(plcType));
    res->rows = res->cols = 1;
    res->data    = malloc(res->rows * sizeof(rawdata*));
    res->data[0] = malloc(res->cols * sizeof(rawdata));
    plc_r_copy_type(&res->types[0], &r_func->res);
    res->names[0] = r_func->res.name;

    if (retval == R_NilValue) {
        res->data[0][0].isnull = 1;
        res->data[0][0].value = NULL;
    } else {
        int ret = 0;
        res->data[0][0].isnull = 0;
        if (r_func->res.conv.outputfunc == NULL) {
            raise_execution_error(plcconn,
                                  "Type %d is not yet supported by R container",
                                  (int)res->types[0].type);
            free_result(res);
            return -1;
        }
        //TODO change output function to take value, not pointer
        ret = r_func->res.conv.outputfunc(&retval, &res->data[0][0].value, &r_func->res);
        if (ret != 0) {
            raise_execution_error(plcconn,
                                  "Exception raised converting function output to function output type %d",
                                  (int)res->types[0].type);
            free_result(res);
            return -1;
        }
    }

    /* send the result back */
    plcontainer_channel_send(conn, (message)res);

    free_result(res);

    return 0;
}
static SEXP arguments_to_r (plcConn *conn, plcRFunction *r_func) {
    SEXP r_args, element;
    int i;

    /* create the argument list */
    PROTECT(r_args = allocVector(VECSXP, r_func->nargs));

    for (i = 0; i < r_func->nargs; i++) {

        if (r_func->call->args[i].data.isnull) {
        	PROTECT(element=R_NilValue);
        	SET_VECTOR_ELT( r_args, i, element );
        	UNPROTECT(1);
        } else {

        	if (r_func->args[i].conv.inputfunc == NULL) {
                raise_execution_error(conn,
                                      "Parameter '%s' type %d is not supported",
                                      r_func->args[i].name,
                                      r_func->args[i].type);
                return NULL;
            }
        	//  this is returned protected by the input function
            element = r_func->args[i].conv.inputfunc(r_func->call->args[i].data.value);
        }
        if (element == NULL) {
            raise_execution_error(conn,
                                  "Converting parameter '%s' to R type failed",
                                  r_func->args[i].name);
            return NULL;
        }

        SET_VECTOR_ELT( r_args, i, element );
    }
    return r_args;
}


SEXP convert_args(callreq req)
{
    SEXP    rargs, element;

    int    i;

    /* create the argument list */
    PROTECT(rargs = allocVector(VECSXP, req->nargs));

    for (i = 0; i < req->nargs; i++) {

        /*
        *  Use \N as null
        */
        if ( req->args[i].data.isnull == TRUE ) {
        	PROTECT(element=R_NilValue);
            SET_VECTOR_ELT( rargs, i, element );
            UNPROTECT(1);
        } else {
            switch( req->args[i].type.type ){

            case PLC_DATA_INT1:
                PROTECT(element = get_r_vector(PLC_DATA_INT1,1));
                LOGICAL(element)[0] = *((bool*)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                UNPROTECT(1);
                break;

            case PLC_DATA_TEXT:
                PROTECT(element = get_r_vector(PLC_DATA_TEXT,1));
                SET_STRING_ELT(element, 0, COPY_TO_USER_STRING(req->args[i].data.value));
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_INT2:
                PROTECT(element = get_r_vector(PLC_DATA_INT2,1));
                INTEGER(element)[0] = *((short *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_INT4:
                PROTECT(element = get_r_vector(PLC_DATA_INT4,1));
                INTEGER(element)[0] = *((int *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_INT8:
                PROTECT(element = get_r_vector(PLC_DATA_INT8,1));
                float tmp = *((int64 *)req->args[i].data.value);
                REAL(element)[0] = tmp ;
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_FLOAT4:
                PROTECT(element = get_r_vector(PLC_DATA_FLOAT4,1));
                REAL(element)[0] = *((float4 *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_FLOAT8:
                PROTECT(element = get_r_vector(PLC_DATA_FLOAT8,1));
                REAL(element)[0] = *((float8 *)req->args[i].data.value);
                SET_VECTOR_ELT( rargs, i, element );
                break;
            case PLC_DATA_ARRAY:
            	PROTECT(element = get_r_array((plcArray *)req->args[i].data.value));
                SET_VECTOR_ELT( rargs, i, element );
                break;

            case PLC_DATA_RECORD:
            case PLC_DATA_UDT:
            default:
                lprintf(ERROR, "unknown type %d", req->args[i].type.type);
            }
        }
    }
    UNPROTECT(1);
    return rargs;
}

static void
pg_get_null(plcDatatype column_type,  SEXP *obj, int elnum)
{
    switch (column_type)
    {
    	case PLC_DATA_INT2:
    	case PLC_DATA_INT4:
    		INTEGER_DATA(*obj)[elnum] = NA_INTEGER;
    		break;
        case PLC_DATA_INT8:
        case PLC_DATA_FLOAT4:
        case PLC_DATA_FLOAT8:
    		NUMERIC_DATA(*obj)[elnum] = NA_REAL;
    		break;
        case PLC_DATA_INT1:
            LOGICAL_DATA(*obj)[elnum] = NA_LOGICAL;
            break;
        case PLC_DATA_TEXT:
        	SET_STRING_ELT(*obj, elnum, NA_STRING);
        	break;
        case PLC_DATA_RECORD:
        case PLC_DATA_UDT:
        case PLC_DATA_ARRAY:
        default:
        	 lprintf(ERROR, "un-handled type %d",column_type);
    }
}

/*
 * given a single non-array pg value, convert to its R value representation
 */
static void
pg_get_one_r(char *value,  plcDatatype column_type, SEXP *obj, int elnum)
{
    switch (column_type)
    {
    /* 2 and 4 byte integer pgsql datatype => use R INTEGER */
        case PLC_DATA_INT2:
            INTEGER_DATA(*obj)[elnum] = *((int16 *)value);
            break;
        case PLC_DATA_INT4:
            INTEGER_DATA(*obj)[elnum] = *((int32 *)value);
            break;

            /*
             * Other numeric types => use R REAL
             * Note pgsql int8 is mapped to R REAL
             * because R INTEGER is only 4 byte
             */
        case PLC_DATA_INT8:
            NUMERIC_DATA(*obj)[elnum] = (int64)(*((float8 *)value));
            break;

        case PLC_DATA_FLOAT4:
            NUMERIC_DATA(*obj)[elnum] = *((float4 *)value);
            break;

        case PLC_DATA_FLOAT8:

            NUMERIC_DATA(*obj)[elnum] = *((float8 *)value);
            break;
        case PLC_DATA_INT1:
            LOGICAL_DATA(*obj)[elnum] = *((int8 *)value);
            break;
        case PLC_DATA_RECORD:
        case PLC_DATA_UDT:
        case PLC_DATA_INVALID:
        case PLC_DATA_ARRAY:
        	lprintf(ERROR, "unhandled type %d", column_type);
        	break;
        case PLC_DATA_TEXT:
        default:
            /* Everything else is defaulted to string */
            SET_STRING_ELT(*obj, elnum, COPY_TO_USER_STRING(*((char **)value)));
    }
}

/*
 * plr_SPI_exec - The builtin SPI_exec command for the R interpreter
 */
SEXP
plr_SPI_exec( SEXP rsql )
{
    const char             *sql;
    SEXP            r_result = NULL,
                    names,
                    row_names,
                    fldvec;

    int             res = 0,
                    i,j;

    char             buf[256];

    sql_msg_statement  msg;
    plcontainer_result result;
    message            resp;

    PROTECT(rsql =  AS_CHARACTER(rsql));
    sql = CHAR(STRING_ELT(rsql, 0));
    UNPROTECT(1);

    if (sql == NULL){
        error("%s", "cannot execute empty query");
        return NULL;
    }


    msg            = pmalloc(sizeof(*msg));
    msg->msgtype   = MT_SQL;
    msg->sqltype   = SQL_TYPE_STATEMENT;
    /*
     * satisfy compiler
     */
    msg->statement = (char *)sql;

    plcontainer_channel_send(plcconn, (message)msg);

    /* we don't need it anymore */
    pfree(msg);

    receive:
    res = plcontainer_channel_receive(plcconn, &resp);
    if (res < 0) {
        lprintf (ERROR, "Error receiving data from the backend, %d", res);
        return NULL;
    }

    switch (resp->msgtype) {
       case MT_CALLREQ:
          handle_call((callreq)resp, plcconn);
          free_callreq((callreq)resp);
          goto receive;
       case MT_RESULT:
           break;
       default:
           lprintf(WARNING, "didn't receive result back %c", resp->msgtype);
           return NULL;
    }

    result = (plcontainer_result)resp;
    if (result->rows == 0){
        return R_NilValue;
    }
    /*
     * r_result is a list of columns
     */
    PROTECT(r_result = NEW_LIST(result->cols));
    /*
     * names for each column
     */
    PROTECT(names = NEW_CHARACTER(result->cols));

    /*
     * we store everything in columns because vectors can only have one type
     * normally we get tuples back in rows with each column possibly a different type,
     * instead we store each column in a single vector
     */

    for (j=0; j<result->cols;j++){
        /*
         * set the names of the column
         */
        SET_STRING_ELT(names, j, Rf_mkChar(result->names[j]));

        //create a vector of the type that is rows long
        PROTECT(fldvec = get_r_vector(result->types[0].type, result->rows));

        for ( i=0; i<result->rows; i++ ){
            /*
             * store the value
             */
            pg_get_one_r(result->data[i][j].value, result->types[0].type, &fldvec, i);
        }

        UNPROTECT(1);
        SET_VECTOR_ELT(r_result, j, fldvec);
    }

    /* attach the column names */
    setAttrib(r_result, R_NamesSymbol, names);

    /* attach row names - basically just the row number, zero based */
    PROTECT(row_names = allocVector(STRSXP, result->rows));

    for ( i=0; i < result->rows; i++ ){
        sprintf(buf, "%d", i+1);
        SET_STRING_ELT(row_names, i, COPY_TO_USER_STRING(buf));
    }

    setAttrib(r_result, R_RowNamesSymbol, row_names);

    /* finally, tell R we are a data.frame */
    setAttrib(r_result, R_ClassSymbol, mkString("data.frame"));

    /*
     * result has
     *
     * an attribute names which is a vector of names
     * a vector of vectors num columns long by num rows
     */
    free_result(result);
    UNPROTECT(3);
    return r_result;
}

void raise_execution_error (plcConn *conn, const char *format, ...) {
    va_list        args;
    error_message  err;
    char          *msg;
    int            len, res;

    if (format == NULL) {
        lprintf(FATAL, "Error message cannot be NULL");
        return;
    }

    va_start(args, format);
    len = 100 + 2 * strlen(format);
    msg = (char*)malloc(len + 1);
    res = vsnprintf(msg, len, format, args);
    if (res < 0 || res >= len) {
        lprintf(FATAL, "Error formatting error message string");
    } else {
        /* an exception to be thrown */
        err             = malloc(sizeof(*err));
        err->msgtype    = MT_EXCEPTION;
        err->message    = msg;
        err->stacktrace = "";

        /* send the result back */
        plcontainer_channel_send(conn, (message)err);
    }

    /* free the objects */
    free(err);
    free(msg);
}

void
throw_pg_notice(const char **msg)
{
    if (msg && *msg)
        last_R_notice = strdup(*msg);
}

void
throw_r_error(const char **msg)
{
    if (msg && *msg)
        last_R_error_msg = strdup(*msg);
    else
        last_R_error_msg = strdup("caught error calling R function");
}
