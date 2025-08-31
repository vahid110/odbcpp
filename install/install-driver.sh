#!/bin/bash
set -e

# ODBCPP Driver Installation Script

DRIVER_NAME="ODBCPP"
INSTALL_DIR="/usr/local/lib"
CONFIG_DIR="/usr/local/etc"

echo "🔧 Installing ODBCPP ODBC Driver..."

# Check if running as root for system installation
if [[ $EUID -ne 0 ]]; then
   echo "❌ This script must be run as root (use sudo)"
   exit 1
fi

# Check if shared library exists
if [ ! -f "libodbcpp.so" ] && [ ! -f "libodbcpp.dylib" ]; then
    echo "❌ Shared library not found. Build with:"
    echo "   cmake -B build"
    echo "   cmake --build build"
    exit 1
fi

# Install shared library
echo "📦 Installing shared library..."
if [ -f "libodbcpp.so" ]; then
    cp libodbcpp.so "$INSTALL_DIR/"
    DRIVER_PATH="$INSTALL_DIR/libodbcpp.so"
elif [ -f "libodbcpp.dylib" ]; then
    cp libodbcpp.dylib "$INSTALL_DIR/"
    DRIVER_PATH="$INSTALL_DIR/libodbcpp.dylib"
fi

chmod 755 "$DRIVER_PATH"
echo "✅ Library installed: $DRIVER_PATH"

# Register driver with ODBC Driver Manager
echo "📋 Registering ODBC driver..."

# Create odbcinst.ini entry
cat > /tmp/odbcpp-driver.ini << EOF
[ODBCPP Redshift]
Description=ODBCPP Driver for Amazon Redshift
Driver=$DRIVER_PATH
Setup=$DRIVER_PATH
FileUsage=1
CPTimeout=
CPReuse=

[ODBCPP PostgreSQL]
Description=ODBCPP Driver for PostgreSQL
Driver=$DRIVER_PATH
Setup=$DRIVER_PATH
FileUsage=1
CPTimeout=
CPReuse=
EOF

# Install driver registration
if command -v odbcinst &> /dev/null; then
    odbcinst -i -d -f /tmp/odbcpp-driver.ini
    echo "✅ Driver registered with ODBC Driver Manager"
else
    echo "⚠️  odbcinst not found. Manual registration required:"
    echo "   Add the following to /etc/odbcinst.ini:"
    cat /tmp/odbcpp-driver.ini
fi

# Clean up
rm -f /tmp/odbcpp-driver.ini

echo ""
echo "🎉 ODBCPP Driver installation complete!"
echo ""
echo "📋 Next steps:"
echo "1. Create DSN entries in /etc/odbc.ini or ~/.odbc.ini"
echo "2. Test with: isql -v DSN_NAME username password"
echo ""
echo "📖 Example DSN configuration:"
echo "[RedshiftProd]"
echo "Driver=ODBCPP Redshift"
echo "Server=your-cluster.redshift.amazonaws.com"
echo "Port=5439"
echo "Database=analytics"
echo "UID=your-username"
echo "PWD=your-password"
echo "SSL=1"
echo ""