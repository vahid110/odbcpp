#!/bin/bash
# Build script for database-specific ODBC drivers

set -e

show_help() {
    echo "Usage: $0 [DATABASE] [OPTIONS]"
    echo ""
    echo "DATABASE options:"
    echo "  redshift     Build Redshift-only ODBC driver"
    echo "  postgresql   Build PostgreSQL-only ODBC driver"
    echo "  mysql        Build MySQL-only ODBC driver"
    echo "  all          Build with all database support (default)"
    echo "  minimal      Build minimal Redshift driver (no examples/tests)"
    echo ""
    echo "OPTIONS:"
    echo "  --debug      Build in Debug mode"
    echo "  --clean      Clean build directory first"
    echo "  --help       Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 redshift              # Redshift-only driver"
    echo "  $0 postgresql --debug    # PostgreSQL driver in debug mode"
    echo "  $0 minimal --clean       # Clean minimal build"
}

DATABASE="all"
BUILD_TYPE="Release"
CLEAN=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        redshift|postgresql|mysql|all|minimal)
            DATABASE="$1"
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

echo "Building ODBC driver for: $DATABASE"
echo "Build type: $BUILD_TYPE"

# Set CMake options based on database choice
case $DATABASE in
    redshift)
        CMAKE_OPTS="-DTARGET_DATABASE=REDSHIFT"
        BUILD_DIR="build-redshift"
        ;;
    postgresql)
        CMAKE_OPTS="-DTARGET_DATABASE=POSTGRESQL"
        BUILD_DIR="build-postgresql"
        ;;
    mysql)
        CMAKE_OPTS="-DTARGET_DATABASE=MYSQL"
        BUILD_DIR="build-mysql"
        ;;
    minimal)
        CMAKE_OPTS="-DTARGET_DATABASE=REDSHIFT -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF"
        BUILD_DIR="build-minimal"
        BUILD_TYPE="MinSizeRel"
        ;;
    all)
        echo "ERROR: 'all' option no longer supported. Choose specific database."
        echo "Available: redshift, postgresql, mysql"
        exit 1
        ;;
esac

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Configure and build
echo "Configuring..."
cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $CMAKE_OPTS

echo "Building..."
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "Build complete!"
echo "Output directory: $BUILD_DIR"
echo "Library: $BUILD_DIR/libodbcpp_core.a"

if [[ "$CMAKE_OPTS" == *"BUILD_EXAMPLES=OFF"* ]]; then
    echo "Examples: Disabled"
else
    echo "Examples: $BUILD_DIR/examples/"
fi