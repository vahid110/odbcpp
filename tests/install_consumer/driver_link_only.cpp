#include <sql.h>
#include <type_traits>
using Allocate = SQLRETURN (SQL_API *)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
static_assert(std::is_same_v<decltype(&SQLAllocHandle), Allocate>);
// Volatile address creates a link relocation; the API is never called.
Allocate volatile exported_allocate_address = &SQLAllocHandle;
int main() { return exported_allocate_address == nullptr; }
