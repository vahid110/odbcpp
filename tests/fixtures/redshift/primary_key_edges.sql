-- OFFLINE PROPOSAL ONLY: no executor or admission authority.
-- Existing odbcpp_fixture schema only; verify USAGE for the named diagnostic user.
-- Refuse any name collision. Record confirmed creations individually.
-- No data, routines, schema/user creation or grants in this file.
CREATE TABLE odbcpp_fixture.m2_pk_none_20261004_c01 (
    payload INTEGER NOT NULL
);
CREATE TABLE odbcpp_fixture."m2_pk_quote_20261004_c01.a""b%'_" (
    "key.a" INTEGER NOT NULL,
    "key""b" INTEGER NOT NULL,
    CONSTRAINT "m2_pk_quote_constraint_20261004_c01.a""b%_"
        PRIMARY KEY ("key""b", "key.a")
);
-- Separately reviewed selective teardown ONLY after confirmed creation/ownership:
-- DROP TABLE odbcpp_fixture."m2_pk_quote_20261004_c01.a""b%'_" RESTRICT;
-- DROP TABLE odbcpp_fixture.m2_pk_none_20261004_c01 RESTRICT;
