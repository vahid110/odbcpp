#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/socket_transport.h"
#include "core/util/deadline.h"
#include <iostream>
#include <iomanip>

void print_bytes(const std::vector<std::byte>& data, const std::string& label) {
  std::cout << label << " (" << data.size() << " bytes): ";
  for (auto b : data) {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
  }
  std::cout << std::dec << "\n";
}

int main() {
  std::cout << "🔍 PostgreSQL Protocol Debug\n";
  std::cout << "============================\n";
  
  try {
    // Create parser and transport
    rs::core::database::postgres::PgProtocolParser parser;
    rs::core::transport::SocketTransport transport;
    
    // Connect
    auto deadline = rs::util::make_deadline(std::chrono::seconds(10));
    std::cout << "🔌 Connecting to Redshift...\n";
    
    auto connect_result = transport.connect("vahidsbr-redshift-cluster.cxzokcavspmr.us-east-1.redshift.amazonaws.com", 5439, deadline);
    if (connect_result.has_error()) {
      std::cout << "❌ Connection failed: " << connect_result.error_message() << "\n";
      return 1;
    }
    std::cout << "✅ TCP connection established\n";
    
    // Create startup message
    std::map<std::string, std::string> params;
    params["user"] = "awsuser";
    params["database"] = "dev";
    
    auto startup_msg = parser.create_startup_message("awsuser", "dev", params);
    print_bytes(startup_msg, "Startup message");
    
    // Send startup message
    auto send_result = transport.send(startup_msg, deadline);
    if (send_result.has_error()) {
      std::cout << "❌ Send failed: " << send_result.error_message() << "\n";
      return 1;
    }
    std::cout << "✅ Startup message sent (" << send_result->n << " bytes)\n";
    
    // Receive response
    std::vector<std::byte> buffer(1024);
    auto recv_result = transport.recv(buffer, deadline);
    if (recv_result.has_error()) {
      std::cout << "❌ Receive failed: " << recv_result.error_message() << "\n";
      return 1;
    }
    
    std::vector<std::byte> response(buffer.begin(), buffer.begin() + recv_result->n);
    print_bytes(response, "Server response");
    
    // Parse response
    auto msg = parser.parse_message(response);
    std::cout << "📨 Message tag: '" << msg.tag << "' (" << static_cast<int>(msg.tag) << ")\n";
    std::cout << "📦 Payload size: " << msg.payload.size() << " bytes\n";
    
    if (parser.is_error_response(msg)) {
      std::cout << "❌ Error response: " << parser.extract_error_message(msg) << "\n";
    } else {
      std::cout << "✅ Non-error response received\n";
      
      // Try to parse as auth request
      auto auth_req = parser.parse_auth_request(msg.payload);
      std::cout << "🔐 Auth type: " << static_cast<int>(auth_req.type) << "\n";
      
      if (auth_req.type == rs::core::database::AuthenticationRequest::Type::MD5) {
        std::cout << "🔐 MD5 authentication required\n";
        
        // Create MD5 auth response
        auto auth_response = parser.create_auth_response(auth_req, "Testing1234", "awsuser");
        print_bytes(auth_response, "MD5 auth response");
        
        // Send auth response
        auto auth_send = transport.send(auth_response, deadline);
        if (auth_send.has_error()) {
          std::cout << "❌ Auth send failed: " << auth_send.error_message() << "\n";
          return 1;
        }
        std::cout << "✅ Auth response sent (" << auth_send->n << " bytes)\n";
        
        // Receive auth result
        auto auth_recv = transport.recv(buffer, deadline);
        if (auth_recv.has_error()) {
          std::cout << "❌ Auth receive failed: " << auth_recv.error_message() << "\n";
          return 1;
        }
        
        std::vector<std::byte> auth_result(buffer.begin(), buffer.begin() + auth_recv->n);
        print_bytes(auth_result, "Auth result");
        
        auto auth_msg = parser.parse_message(auth_result);
        std::cout << "📨 Auth result tag: '" << auth_msg.tag << "' (" << static_cast<int>(auth_msg.tag) << ")\n";
        
        if (parser.is_error_response(auth_msg)) {
          std::cout << "❌ Auth failed: " << parser.extract_error_message(auth_msg) << "\n";
        } else {
          std::cout << "✅ Authentication successful!\n";
        }
      }
    }
    
    transport.close();
    std::cout << "🧹 Connection closed\n";
    
  } catch (const std::exception& e) {
    std::cout << "❌ Exception: " << e.what() << "\n";
    return 1;
  }
  
  return 0;
}