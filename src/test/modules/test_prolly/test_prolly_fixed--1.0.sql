/* src/test/modules/test_prolly/test_prolly_fixed--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_fixed" to load this file. \quit

CREATE DOMAIN @extschema@.posint AS int CHECK (VALUE > 0);
