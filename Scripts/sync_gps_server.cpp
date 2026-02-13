#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>

std::string getLatestDmesgLine() {
    FILE* pipe = popen("sudo /bin/dmesg | grep 'GPIO_16_IRQ' | tail -1", "r");
    if (!pipe) return "ERROR";
    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

long extractTimestamp(const std::string& dmesgLine) {
    size_t pos = dmesgLine.find("GPIO_16_IRQ:");
    if (pos == std::string::npos) return -1;
    std::string ts_str = dmesgLine.substr(pos + 12);
    return std::stod(ts_str);
}

int main() {
    int server_fd, new_socket;
    sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[1024] = {0};

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(12345);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);

    while (true) {
        new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
        std::cout << "Request received from client." << std::endl;
        read(new_socket, buffer, 1024); // Read trigger
        std::string msg = std::string(buffer);
        std::string response;
        response = getLatestDmesgLine();
        double time_Pi = extractTimestamp(response);
        printf("Sending timestamps %+f ns\n\n", time_Pi);
        send(new_socket, response.c_str(), response.size(), 0);
        close(new_socket);
    }
}