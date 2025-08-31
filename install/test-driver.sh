#!/bin/bash
set -e

# ODBCPP Driver Test Installation (User-level)

echo "🔧 Testing ODBCPP ODBC Driver installation..."

# Check if shared library exists
if [ ! -f "libodbcpp.dylib" ]; then
    echo "❌ Shared library not found in current directory"
    exit 1
fi

echo "✅ Found libodbcpp.dylib"

# Check dependencies
echo "📋 Checking library dependencies:"
otool -L libodbcpp.dylib

# Create test DSN
echo ""
echo "📝 Creating test DSN in ~/.odbc.ini..."

mkdir -p ~/.config/odbc
cat >> ~/.odbc.ini << 'EOF'

[RedshiftTest]
Driver=ODBCPP Redshift
Server=vahidsbr-redshift-cluster.cxzokcavspmr.us-east-1.redshift.amazonaws.com
Port=5439
Database=dev
UID=awsuser
PWD=Testing1234
SSL=0
Description=Test Redshift connection via ODBCPP

EOF

echo "✅ DSN created in ~/.odbc.ini"

# Show what would be registered
echo ""
echo "📋 Driver registration that would be added to /etc/odbcinst.ini:"
echo ""
cat << EOF
[ODBCPP Redshift]
Description=ODBCPP Driver for Amazon Redshift
Driver=$(pwd)/libodbcpp.dylib
Setup=$(pwd)/libodbcpp.dylib
FileUsage=1
CPTimeout=
CPReuse=
EOF

echo ""
echo "🎉 Test setup complete!"
echo ""
echo "📋 To complete full installation:"
echo "1. Copy libodbcpp.dylib to /usr/local/lib/"
echo "2. Add driver registration to /etc/odbcinst.ini"
echo "3. Test with: isql -v RedshiftTest"