#include "core/database/database_factory.h"
#include <iostream>

int main() {
    std::cout << "🔍 Debug Prepared Statement Protocol\n";
    std::cout << "====================================\n";
    
    try {
        auto conn = rs::core::database::DatabaseFactory::create_connection();
        
        rs::core::database::ConnectionSettings settings;
        settings.host = "redshift-cluster-1.cjqhqhqhqhqh.us-east-1.redshift.amazonaws.com";
        settings.port = 5439;
        settings.user = "testuser";
        settings.password = "TestPass123";
        settings.database = "dev";
        settings.use_ssl = false;
        
        std::cout << "Connecting...\n";
        auto connect_result = conn->connect(settings);
        if (connect_result.has_error()) {
            std::cout << "❌ Connection failed: " << connect_result.error_message() << "\n";
            return 1;
        }
        std::cout << "✅ Connected\n";
        
        // Test simple query first
        std::cout << "\n📋 Testing simple query...\n";
        auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
        auto simple_result = conn->execute_query("SELECT 'Hello' as message, 123 as number", deadline);
        if (simple_result.has_error()) {
            std::cout << "❌ Simple query failed: " << simple_result.error_message() << "\n";
        } else {
            std::cout << "✅ Simple query succeeded\n";
            for (const auto& row : simple_result->rows) {
                for (const auto& col : row) {
                    std::cout << "  " << col << " ";
                }
                std::cout << "\n";
            }
        }
        
        // Test prepared query
        std::cout << "\n📋 Testing prepared query...\n";
        std::vector<std::string> params = {"Hello Prepared!", "456"};
        auto prepared_result = conn->execute_prepared("SELECT $1 as message, $2 as number", params, deadline);
        if (prepared_result.has_error()) {
            std::cout << "❌ Prepared query failed: " << prepared_result.error_message() << "\n";
        } else {
            std::cout << "✅ Prepared query succeeded\n";
            for (const auto& row : prepared_result->rows) {
                for (const auto& col : row) {
                    std::cout << "  " << col << " ";
                }
                std::cout << "\n";
            }
        }
        
    } catch (const std::exception& e) {
        std::cout << "❌ Exception: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}