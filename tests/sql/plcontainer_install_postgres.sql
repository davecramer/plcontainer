-- Creating PL/Container trusted language

CREATE OR REPLACE FUNCTION plcontainer_call_handler()
RETURNS LANGUAGE_HANDLER
AS '$libdir/plcontainer' LANGUAGE C;

CREATE OR REPLACE TRUSTED LANGUAGE plcontainer HANDLER plcontainer_call_handler;

-- Defining container configuration management functions


CREATE OR REPLACE FUNCTION plcontainer_read_local_config(verbose bool) RETURNS text
AS '$libdir/plcontainer', 'read_plcontainer_config'
LANGUAGE C VOLATILE;


CREATE OR REPLACE FUNCTION plcontainer_read_config() RETURNS text AS $$
    select plcontainer_read_local_config(false);
$$ LANGUAGE SQL VOLATILE;
