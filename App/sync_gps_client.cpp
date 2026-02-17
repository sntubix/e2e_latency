#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <iostream>
#include <fstream>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <filesystem>


#define GPIO_CHIP "/dev/gpiochip4"
#define GPIO_OUTPUT 17

#define ITERATIONS 1800 // Adjust to number of iteration wanted
#define SLEEP_S 0
#define SLEEP_NS 500000000

std::vector<double> table_offset;
std::vector<double> table_jitter;
std::vector<double> mono_table;
std::vector<double> Signal_time_one;
std::vector<double> Signal_time_two;
std::vector<double> Signal_time_offset;
long mem_offset;

// Transform clockid_t into ns
double get_time_ns(clockid_t clock_type) {
    struct timespec ts;
    clock_gettime(clock_type, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

// Search kernel ring buffer for new interrupt from bottom
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

// check if file name is used and add number to it
std::string getFilename(const std::string& baseName) {
    std::string filename = baseName;
    int counter = 0;
    do
    {
        filename = baseName + "_" + std::to_string(counter) + ".csv";
        ++counter;
    }
    while (std::filesystem::exists(filename));

    return filename;
}

// send the timestamp request from other pi
std::string requestDmesgFromPi2(const std::string& ip) {
    int sock = 0;
    sockaddr_in serv_addr;
    char buffer[1024] = {0};

    sock = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(12345);
    inet_pton(AF_INET, ip.c_str(), &serv_addr.sin_addr);

    connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    send(sock, "GET", strlen("GET"), 0);
    read(sock, buffer, 1024);
    close(sock);

    return std::string(buffer);
}

// Parse ting buffer line to only get timestamps
double extractTimestamp(const std::string& dmesgLine) {
    size_t pos = dmesgLine.find("GPIO_16_IRQ:");
    if (pos == std::string::npos) return -1;
    std::string ts_str = dmesgLine.substr(pos + 12);
    return std::stod(ts_str);
}

// Compare timestamps and register store them
int checkSynchronisation(int &n_meas, double &trigger)
{
    // Extract local timestamps
    std::string response = getLatestDmesgLine();
    double time_Pi_two = extractTimestamp(response);

    // Ask other Pi for timestamps
    std::string dmesgLine = requestDmesgFromPi2("10.19.108.184");
    double time_Pi_one = extractTimestamp(dmesgLine);

    // Compute offset
    double offset = time_Pi_two - time_Pi_one;
    double jitter = offset - mem_offset;
    mem_offset = offset;

    // Compute times between gpio trigger and registered timestamps
    double trigger_pi_two = time_Pi_two - trigger;
    double trigger_pi_one = time_Pi_one - trigger;
    double trigger_offset = trigger_pi_one - trigger_pi_two;

    Signal_time_one.push_back(trigger_pi_one);
    Signal_time_two.push_back(trigger_pi_two);
    Signal_time_offset.push_back(trigger_offset);
    table_offset.push_back(offset);
    table_jitter.push_back(jitter);
    printf("    Synchronisation offset = %+f ns, jitter = %+f ns\n\n", offset, jitter);
    return 0;
}

// Compute minimum and maximum from a table
void getMinMaxFromTable(std::vector<double> &table, std::string table_name)
{
    auto getMinMaxAbsSigned = [](const std::vector<double>& table) {
        auto min_it = std::min_element(table.begin(), table.end(),
            [](double a, double b) { return std::labs(a) < std::labs(b); });

        auto max_it = std::max_element(table.begin(), table.end(),
            [](double a, double b) { return std::labs(a) < std::labs(b); });

        return std::make_pair(*min_it, *max_it);
    };

    auto [min_signed, max_signed] = getMinMaxAbsSigned(table);

    std::cout << "Min " << table_name << " (abs): " << min_signed << "\n";
    std::cout << "Max " << table_name << " (abs): " << max_signed << "\n";
}

int main() {
    double start_time = 0L;
    struct timespec sleep_time;
    double hour_time = 0L;
    int pin_value = 1;
    int i = ITERATIONS;

    // Access gpio chip
    struct gpiod_chip *chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        std::cerr << "Failed to open gpiochip4\n";
        return 1;
    }
    else{
        std::cout << "Open gpiochip4\n";
    }

    // Access gpio 17
    struct gpiod_line *line_out = gpiod_chip_get_line(chip, GPIO_OUTPUT);
    if (!line_out) {
        std::cerr << "Failed to get GPIO line 17\n";
        gpiod_chip_close(chip);
        return 1;
    }
    else{
        std::cout << "Get GPIO 17\n";
    }

    // Configure gpio 17 as output
    int ret = gpiod_line_request_output(line_out, "gpio_out", 0);
    if (ret < 0) {
        std::cerr << "Failed to request line as output\n";
        gpiod_chip_close(chip);
        return 1;
    }
    else{
        std::cout << "Reaquest GPIO 17 line\n";
    }

    sleep_time.tv_sec = SLEEP_S;
    sleep_time.tv_nsec = SLEEP_NS;
    std::cout << "Initiate Timestamps" << std::endl;
    hour_time = get_time_ns(CLOCK_REALTIME);
    std::cout << "hour_time: " << hour_time <<std::endl;
    int count_ones = 0;
    int count_zeros = 0;
    int meas = 0;
    mem_offset = 0L;
    double trigger_time = 0;
    while (i) {
        double time_ns = get_time_ns(CLOCK_REALTIME);
        std::cout << "time_ns: " << time_ns <<std::endl;

		// Get elapsed during sleep
        gpiod_line_set_value(line_out, pin_value);
        meas = (ITERATIONS - i)/2;
        if(pin_value == 0)
        {
            trigger_time = time_ns;
            count_zeros++;
            printf("Measurement %d: Pin value = %d; Time = %f ns\n", meas, pin_value, time_ns);
            mono_table.push_back(time_ns);
        }
        else if (pin_value == 1 && i < ITERATIONS)
        {
            count_ones++;
            int ret = checkSynchronisation(meas, trigger_time);
            if(ret)
            {
                std::cerr << "An issue occured in synchronisation - check logs\n";
                return 1;
            }
        } 
        else
        {
            // Start nothing to measure yet
        }
        pin_value = 1 - pin_value;
        nanosleep(&sleep_time, NULL);
        i--;
    }
    if(count_ones < count_zeros)
    {
        int ret = checkSynchronisation(meas, trigger_time);
        if(ret)
        {
            std::cerr << "An issue occured in synchronisation - check logs\n";
            return 1;
        }
    }
    // First jitter is from init to first offset so it is not relevant
    auto first_jitter = table_jitter.begin()+1;
    std::string build = "co_ref";
    auto first_offset = table_offset.begin();
    double end_time = mono_table.back();
    printf("end time = %f ns\n", end_time);
    printf("start time = %f ns\n", hour_time);
    double final_time = end_time - hour_time;
    double meas_time = static_cast<double>(final_time) / 1000000000.0;
    printf("Measurement Time : %.4f s\n", meas_time);

    std::string table_name = "offset";
    double sum;
    double avg;
    // Compute offset metrics
    if (!table_offset.empty())
    {
        sum = std::accumulate(first_offset, table_offset.end(), 0);
        avg = sum / table_offset.size();
        getMinMaxFromTable(table_offset, table_name);
        printf("Average offset : %.3f ns\n", avg);
    }
    else
    {
        printf("Offset Table is empty");
    }

    // Compute jitter metrics
    if (!table_jitter.empty())
    {
        table_name = "jitter";
        sum = std::accumulate(first_jitter, table_jitter.end(), 0);
        avg = sum / table_jitter.size();
        getMinMaxFromTable(table_jitter, table_name);
        printf("Average Jitter : %.3f ns\n", avg);
    }
    else
    {
        printf("Jitter Table is empty");
    }

    // Export results into a csv
    int duration = static_cast<int>(meas_time);
    std::string path = "/home/francois.provost/latency_test/test_results/";
    std::string baseFilename = path + "synchronisation_test_gps_" + 
                               std::to_string(duration) + "s";

    std::string filename = getFilename(baseFilename);

    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Erreur lors de l'ouverture du fichier : " << filename << std::endl;
        return 1;
    }

    file << "Time" << ",";
    file << "Offsets" << ",";
    file << "Jitter" << ",";
    file << "Time Pi 1" << ",";
    file << "Time Pi 2" << ",";
    file << "Time Pi Offsets" << "\n";

    size_t maxSize = mono_table.size();
    for (size_t i = 0; i < maxSize; ++i) {
        file << (i < mono_table.size() ? std::to_string(mono_table[i]-start_time) : "") << ",";
        file << (i < table_offset.size() ? std::to_string(table_offset[i]) : "") << ",";
        file << (i < table_jitter.size() ? std::to_string(table_jitter[i]) : "") << ",";
        file << (i < Signal_time_one.size() ? std::to_string(Signal_time_one[i]) : "") << ",";
        file << (i < Signal_time_two.size() ? std::to_string(Signal_time_two[i]) : "") << ",";
        file << (i < Signal_time_offset.size() ? std::to_string(Signal_time_offset[i]) : "") << "\n";
    }

    file.close();
    std::cout << "Fichier exporté : " << filename << std::endl;

    gpiod_line_release(line_out);
    gpiod_chip_close(chip);
    return 0;
}