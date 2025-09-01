#!/bin/bash
#
# ODBC Test Environment Setup Script
#
# This script configures the ODBC environment for testing the ODBCPP driver.
# It sets up the required environment variables (ODBCINI and ODBCSYSINI) to
# point to the local ODBC configuration files.
#
# USAGE:
#   # Source the script to set environment variables in current shell:
#   source ./setup-test-env.sh
#
#   # Or run directly to see configuration status:
#   ./setup-test-env.sh
#
# WHAT IT DOES:
#   - Sets ODBCINI to point to local odbc.ini (data source definitions)
#   - Sets ODBCSYSINI to point to local directory (driver definitions)
#   - Verifies configuration files exist
#   - Lists available DSNs for testing
#
# REQUIRED FILES:
#   - odbc.ini: Contains DSN definitions (RedshiftTest, PostgreSQLTest, etc.)
#   - odbcinst.ini: Contains ODBCPP driver definition
#
# NOTE: This overrides system-wide ODBC configuration for testing purposes.

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set ODBC configuration paths
export ODBCINI="${SCRIPT_DIR}/odbc.ini"
export ODBCSYSINI="${SCRIPT_DIR}"

echo "ODBC Test Environment Setup:"
echo "ODBCINI=${ODBCINI}"
echo "ODBCSYSINI=${ODBCSYSINI}"

# Verify files exist
if [ -f "${ODBCINI}" ]; then
    echo "✓ odbc.ini found"
else
    echo "✗ odbc.ini not found at ${ODBCINI}"
fi

if [ -f "${ODBCSYSINI}/odbcinst.ini" ]; then
    echo "✓ odbcinst.ini found"
else
    echo "✗ odbcinst.ini not found at ${ODBCSYSINI}/odbcinst.ini"
fi

echo ""
echo "Available DSNs:"
if [ -f "${ODBCINI}" ]; then
    grep "^\[" "${ODBCINI}" | grep -v "ODBC Data Sources" | sed 's/\[//g' | sed 's/\]//g'
fi

echo ""
echo "To use this environment in your shell:"
echo "source ${SCRIPT_DIR}/setup-test-env.sh"