#include "postgres.h"
#include "utils/array.h"

#include "plcontainer.h"
#include "plc_typeio.h"
#include "message_fns.h"
#include "common/comm_utils.h"

static rawdata *plc_backend_array_next(plcIterator *self);

Datum get_array_datum(plcArray *arr, plcTypeInfo *ret_type)
{
    Datum         dvalue;
    Datum	     *elems;
    ArrayType    *array = NULL;
    int          *lbs = NULL;
    int           i;
    MemoryContext oldContext;

    lbs = (int*)palloc(arr->meta->ndims * sizeof(int));
    for (i = 0; i < arr->meta->ndims; i++)
        lbs[i] = 1;

    elems = palloc(sizeof(Datum) * arr->meta->size);
    for (i = 0; i < arr->meta->size; i++) {
        elems[i] = Int64GetDatum( ((long long*)arr->data)[i] );
    }

    oldContext = MemoryContextSwitchTo(pl_container_caller_context);
    array = construct_md_array(elems,
                               arr->nulls,
                               arr->meta->ndims,
                               arr->meta->dims,
                               lbs,
                               ret_type->subTypes[0].typeOid,
                               ret_type->subTypes[0].typlen,
                               ret_type->subTypes[0].typbyval,
                               ret_type->subTypes[0].typalign);

    dvalue = PointerGetDatum(array);
    MemoryContextSwitchTo(oldContext);

    pfree(lbs);
    pfree(elems);

    return dvalue;
}

/*
void get_tuple_store( MemoryContext oldContext, MemoryContext messageContext,
        ReturnSetInfo *rsinfo, plcontainer_result res, int *isNull )
{
    AttInMetadata      *attinmeta;
    Tuplestorestate    *tupstore = tuplestore_begin_heap(true, false, work_mem);
    TupleDesc          tupdesc;
    HeapTuple    typetup,
                 tuple;
    Form_pg_type type;
    Oid          typeOid;
    int32        typeMod;

    char **values;
    int i,j;

     * TODO: Returning tuple, you will not have any tuple description for the
     * function returning setof record. This needs to be fixed *
     * get the requested return tuple description *
    if (rsinfo->expectedDesc != NULL)
        tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
    else {
        elog(ERROR, "Functions returning 'record' type are not supported yet");
        *isNull = TRUE;
        return;
    }

    for (j = 0; j < res->cols; j++) {
        parseTypeString(res->types[j], &typeOid, &typeMod);
        typetup = SearchSysCache(TYPEOID, typeOid, 0, 0, 0);

        if (!HeapTupleIsValid(typetup)) {
            MemoryContextSwitchTo(oldContext);
            MemoryContextDelete(messageContext);
            elog(FATAL, "[plcontainer] Invalid heaptuple at result return");
            // This won`t run
            *isNull=TRUE;
        }

        type = (Form_pg_type)GETSTRUCT(typetup);

        strcpy(tupdesc->attrs[j]->attname.data, res->names[j]);
        tupdesc->attrs[j]->atttypid = typeOid;
        ReleaseSysCache(typetup);
    }

    attinmeta = TupleDescGetAttInMetadata(tupdesc);

    * OK, go to work *
    rsinfo->returnMode = SFRM_Materialize;
    MemoryContextSwitchTo(oldContext);

     *
     * SFRM_Materialize mode expects us to return a NULL Datum. The actual
     * tuples are in our tuplestore and passed back through
     * rsinfo->setResult. rsinfo->setDesc is set to the tuple description
     * that we actually used to build our tuples with, so the caller can
     * verify we did what it was expecting.
     *
    rsinfo->setDesc = tupdesc;

    for (i=0; i<res->rows;i++){

        values = palloc(sizeof(char *)* res->cols);
        for (j=0; j< res->cols;j++){
            values[j] = res->data[i][j].value;
        }

        * construct the tuple *
        tuple = BuildTupleFromCStrings(attinmeta, values);
        pfree(values);

        * switch to appropriate context while storing the tuple *
        oldContext = MemoryContextSwitchTo(messageContext);

        * now store it *
        tuplestore_puttuple(tupstore, tuple);

        MemoryContextSwitchTo(oldContext);
    }
    rsinfo->setResult = tupstore;
    MemoryContextSwitchTo(oldContext);
    MemoryContextDelete(messageContext);

    *isNull = TRUE;
}
*/

plcIterator *init_array_iter(Datum d, plcTypeInfo *argType) {
    ArrayType    *array = DatumGetArrayTypeP(d);
    plcIterator  *iter;
    plcArrayMeta *meta;
    int           i;

    iter = (plcIterator*)palloc(sizeof(plcIterator));
    meta = (plcArrayMeta*)palloc(sizeof(plcArrayMeta));
    iter->meta = meta;

    meta->type = argType->subTypes[0].type;
    meta->ndims = ARR_NDIM(array);
    meta->dims = (int*)palloc(meta->ndims * sizeof(int));
    iter->position = (char*)palloc(sizeof(int) * meta->ndims * 2 + 2);
    ((plcTypeInfo**)iter->position)[0] = argType;
    meta->size = meta->ndims > 0 ? 1 : 0;
    for (i = 0; i < meta->ndims; i++) {
        meta->dims[i] = ARR_DIMS(array)[i];
        meta->size *= ARR_DIMS(array)[i];
        ((int*)iter->position)[i + 2] = ARR_LBOUND(array)[i];
        ((int*)iter->position)[i + meta->ndims + 2] = ARR_LBOUND(array)[i];
    }
    iter->data = (char*)array;
    iter->next = plc_backend_array_next;
    iter->cleanup =  NULL;

    return iter;
}

char *fill_type_value(Datum funcArg, plcTypeInfo *argType) {
    char *out = NULL;
    switch(argType->type) {
        case PLC_DATA_INT1:
            out = (char*)pmalloc(1);
            *((char*)out) = DatumGetBool(funcArg);
            break;
        case PLC_DATA_INT2:
            out = (char*)pmalloc(2);
            *((int16*)out) = DatumGetInt16(funcArg);
            break;
        case PLC_DATA_INT4:
            out = (char*)pmalloc(4);
            *((int32*)out) = DatumGetInt32(funcArg);
            break;
        case PLC_DATA_INT8:
            out = (char*)pmalloc(8);
            *((int64*)out) = DatumGetInt64(funcArg);
            break;
        case PLC_DATA_FLOAT4:
            out = (char*)pmalloc(4);
            *((float4*)out) = DatumGetFloat4(funcArg);
            break;
        case PLC_DATA_FLOAT8:
            out = (char*)pmalloc(8);
            *((float8*)out) = DatumGetFloat8(funcArg);
            break;
        case PLC_DATA_TEXT:
            out = DatumGetCString(OidFunctionCall1(argType->output, funcArg));
            break;
        case PLC_DATA_ARRAY:
            out = (char*)init_array_iter(funcArg, argType);
            break;
        case PLC_DATA_RECORD:
        case PLC_DATA_UDT:
        default:
            lprintf(ERROR, "Type %d is not yet supported by PLcontainer", (int)argType->type);
    }
    return out;
}

static rawdata *plc_backend_array_next(plcIterator *self) {
    plcArrayMeta *meta;
    ArrayType    *array;
    plcTypeInfo  *typ;
    int          *lbounds;
    int          *pos;
    int           dim;
    bool          isnull = 0;
    Datum         el;
    rawdata      *res;

    res     = palloc(sizeof(rawdata));
    meta    = (plcArrayMeta*)self->meta;
    array   = (ArrayType*)self->data;
    typ     = ((plcTypeInfo**)self->position)[0];
    lbounds = (int*)self->position + 2;
    pos     = (int*)self->position + 2 + meta->ndims;

    el = array_ref(array, meta->ndims, pos, typ->typlen,
                   typ->subTypes[0].typlen, typ->subTypes[0].typbyval,
                   typ->subTypes[0].typalign, &isnull);
    if (isnull) {
        res->isnull = 1;
        res->value  = NULL;
    } else {
        res->isnull = 0;
        res->value = fill_type_value(el, &typ->subTypes[0]);
    }

    dim     = meta->ndims - 1;
    while (dim >= 0 && pos[dim]-lbounds[dim] < meta->dims[dim]) {
        pos[dim] += 1;
        if (pos[dim]-lbounds[dim] >= meta->dims[dim]) {
            pos[dim] = lbounds[dim];
            dim -= 1;
        } else {
            break;
        }
    }

    return res;
}