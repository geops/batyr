--
-- PostgreSQL database dump
--

SET statement_timeout = 0;
SET client_encoding = 'SQL_ASCII';
SET standard_conforming_strings = on;
SET check_function_bodies = false;
SET client_min_messages = warning;

--
-- Name: test; Type: SCHEMA; Schema: -; Owner: batyr
--

CREATE SCHEMA test;


ALTER SCHEMA test OWNER TO batyr;

SET search_path = test, pg_catalog;

SET default_tablespace = '';

SET default_with_oids = false;

--
-- Name: africa; Type: TABLE; Schema: test; Owner: batyr; Tablespace: 
--

CREATE TABLE africa (
    id character varying(4) NOT NULL,
    code character varying(4),
    country character varying(35),
    the_geom public.geometry,
    CONSTRAINT enforce_dims_the_geom CHECK ((public.st_ndims(the_geom) = 2)),
    CONSTRAINT enforce_srid_the_geom CHECK ((public.st_srid(the_geom) = (0)))
);


ALTER TABLE test.africa OWNER TO batyr;

--
-- Name: dataset1; Type: TABLE; Schema: test; Owner: batyr; Tablespace: 
--

CREATE TABLE dataset1 (
    id integer NOT NULL,
    title text,
    ts timestamp without time zone,
    t time without time zone,
    d date,
    dbl double precision,
    geom public.geometry
);


ALTER TABLE test.dataset1 OWNER TO batyr;

--
-- Name: dataset1_id_seq; Type: SEQUENCE; Schema: test; Owner: batyr
--

CREATE SEQUENCE dataset1_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER TABLE test.dataset1_id_seq OWNER TO batyr;

--
-- Name: dataset1_id_seq; Type: SEQUENCE OWNED BY; Schema: test; Owner: batyr
--

ALTER SEQUENCE dataset1_id_seq OWNED BY dataset1.id;


--
-- Name: id; Type: DEFAULT; Schema: test; Owner: batyr
--

ALTER TABLE ONLY dataset1 ALTER COLUMN id SET DEFAULT nextval('dataset1_id_seq'::regclass);


--
-- Name: dataset1_pkey; Type: CONSTRAINT; Schema: test; Owner: batyr; Tablespace: 
--

ALTER TABLE ONLY dataset1
    ADD CONSTRAINT dataset1_pkey PRIMARY KEY (id);


--
-- Name: pk_africa; Type: CONSTRAINT; Schema: test; Owner: batyr; Tablespace: 
--

ALTER TABLE ONLY africa
    ADD CONSTRAINT pk_africa PRIMARY KEY (id);


--
-- PostgreSQL database dump complete
--

