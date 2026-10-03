-- OFFLINE PROPOSAL ONLY: see README.md before any separately admitted execution.
-- Existing schema: odbcpp_fixture. No schema creation or privilege changes.
-- Confirm all three names (including every procedure overload) are absent first.
-- Stop on any error; never retry automatically or delete a colliding object.
-- Record each confirmed creation for selective teardown after partial failure.
-- No data is inserted and the procedure must never be invoked.

-- Column order is key_a, key_b; declared primary-key order is key_b, key_a.
CREATE TABLE odbcpp_fixture.m2_catalog_parent_20261003_c01 (
    key_a INTEGER NOT NULL,
    key_b INTEGER NOT NULL,
    PRIMARY KEY (key_b, key_a)
);

-- Column order is ref_a, ref_b; foreign-key order is ref_b, ref_a.
-- Expected pair sequence: ref_b -> key_b (1), ref_a -> key_a (2).
CREATE TABLE odbcpp_fixture.m2_catalog_child_20261003_c01 (
    ref_a INTEGER NOT NULL,
    ref_b INTEGER NOT NULL,
    FOREIGN KEY (ref_b, ref_a)
        REFERENCES odbcpp_fixture.m2_catalog_parent_20261003_c01 (key_b, key_a)
);

-- Metadata fixture only: argument order/modes are IN INTEGER, INOUT INTEGER.
-- No replacement, overload creation, definer privileges, or procedure execution.
CREATE PROCEDURE odbcpp_fixture.sp_m2_catalog_modes_20261003_c01 (
    p_input IN INTEGER,
    p_result INOUT INTEGER
)
AS $$
BEGIN
    NULL;
END;
$$ LANGUAGE plpgsql SECURITY INVOKER;

-- TEARDOWN PROPOSAL (commented out to prevent whole-file execution dropping
-- newly created fixtures). Execute separately only for objects proven created
-- by this admission, after confirming ownership/signature and no foreign use.
-- Child first; RESTRICT refuses unexpected dependencies. Do not use CASCADE.
-- DROP PROCEDURE odbcpp_fixture.sp_m2_catalog_modes_20261003_c01 (INTEGER, INTEGER);
-- DROP TABLE odbcpp_fixture.m2_catalog_child_20261003_c01 RESTRICT;
-- DROP TABLE odbcpp_fixture.m2_catalog_parent_20261003_c01 RESTRICT;
