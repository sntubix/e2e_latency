# Motion-to-Motion, Glass-to-Glass and End-to-End Latency Measurement System for Raspberry Pi  

The system includes:

- Raspberry Pi Linux installation and configuration 
- custom Linux kernel module for movement detection and GPIO interrupt handling
- Chrony and GPS based time synchronization
- Client/server C++ synchronization test applications  
- Automated launch and export of GPIO interrupt timestamps  

This README provides everything needed to install, configure, and run the full system.

### Abbreviations
- RPI : Raspberry Pi
- GPIO : General Purpose Input Output
- V2X : Vehicle to everything

---

# 1. Repository Structure

---

```
.
├── App/
|   ├── e2e_extract.py
│   ├── sync_gps_client.cpp
│   └── sync_gps_server.cpp
├── Module/
│   ├── Sync
|   |  ├── gpio16_irq.c
|   |  ├── gpio16_irq.dts
|   |  └── Makefile
│   ├── e2e_module.c
│   ├── e2e_module.dts
|   └── Makefile
├── Results/
│   ├── Baseline
|   |   └── Baseline.csv
│   └── Field tests
|       ├── Field_Test_4G.csv
|       └── Field_Test_5G.csv
├── Images/
│   └── Images content in README.md
└── README.md
```

---

# 2. Install Ubuntu Server

---

### Install [PI Imager](https://www.raspberrypi.com/software/)

- Configuration
  <table>
  <tr>
    <td style="border-right:1px solid #888;">Raspberry Pi device</th>
    <td>Raspberry Pi 5</th>
  </tr>
  <tr>
    <td style="border-right:1px solid #888;">Operating System</td>
    <td> Other general-purpose OS 
        <ul>
            <li> Ubuntu
                <ul>
                    <li> Ubuntu Server 24.04.3 LTS (64-bit)
                </ul>
            </li>
        </ul>
    </td>        
  </tr>
  <tr>
    <td style="border-right:1px solid #888;">Storage</td>
    <td>SD Card</th>
  </tr>
  </table>

  ![alt text](images/PI_imager.png)


### Choose Edit settings

- Configure user name and password

  ![alt text](images/Pi_user_setting.png)

- Generate SSH key
  ```bash
  ssh-keygen -t ed25519 -C "your_email"
  ```

- Add SSH key for remote access

  ![alt text](images/ssh_setting.png)

### Remote access

- Get Raspberry Pi ip address
  ```bash
  hostname -I
  ```

- Use IP and user name for remote access

  ```bash
  ssh pi_user_name@Pi_ip_address
  ```

- **Note: Computer and RPI should be on the same network**

---

# 3. Required Packages

---

- Install the required system packages :
  ```bash
  sudo apt install build-essential libgpiod-dev rt-tests linux-tools-common linux-tools-$(uname -r) wireless-tools gpiod chrony network-manager linux-headers-$(uname -r)
  ```
  ### Package Overview
  | Package | Purpose |
  |--------|----------|
  | **build-essential** | GCC/G++ and essential build tools |
  | **libgpiod-dev**, **gpiod** | GPIO control library and CLI |
  | **rt-tests** | Includes `cyclictest` for real-time analysis |
  | **wireless-tools**, **network-manager** | Network utilities |
  | **linux-tools-\***, **linux-headers-\*** | Required for kernel module |
  | **chrony** | NTP-based time synchronization tool |

---

# 4. Clock Synchronization

---

### Check status
  ```bash
  systemctl status chronyd
  ```
### Start Chrony and enable at boot
  ```bash
  sudo systemctl start chronyd
  systemctl enable chronyd
  ```

### Restart Chrony
  ```bash
  systemctl restart chrony
  ```

### Check Chrony sources
  ```bash
  chronyc sources
  ```
### Check Chrony tracking
  ```bash
  chronyc tracking
  ```
  
## 4.1. Configure GPS
### Configure Chrony for GPS synchronisation
- open chrony config file
  ```bash
  sudo nano /etc/chrony/chrony.conf
  ```
- Replace NTP server config by GPS
  ```text
  # NMEA via gpsd SHM (unit 0)
  refclock SHM 0 refid NMEA poll 4 offset 0.241 delay 0.2 noselect

  # PPS from kernel
  refclock PPS /dev/pps0 lock NMEA refid PPS poll 2

  # This directive specify the file into which chronyd will store the rate
  # information.
  driftfile /var/lib/chrony/chrony.drift

  # Save NTS keys and cookies.
  #ntsdumpdir /var/lib/chrony

  # Uncomment the following line to turn logging on.
  log tracking measurements statistics

  # Log files location.
  logdir /var/log/chrony

  # Stop bad estimates upsetting machine clock.
  maxupdateskew 100.0

  # This directive enables kernel synchronisation (every 11 minutes) of the
  # real-time clock. Note that it can't be used along with the 'rtcfile' directive.
  rtcsync

  # Step the system clock instead of slewing it if the adjustment is larger than
  # one second, but only in the first three clock updates.
  makestep 1 3

  # Get TAI-UTC offset and leap seconds from the system tz database.
  # This directive must be commented out when using time sources serving
  # leap-smeared time.
  leapsectz right/UTC
  ```
- Add gpio18 (or other) as PPS signal source in boot config file
  ```bash
  sudo nano /boot/firmware/config.txt
  ```
  ```text
  # PPS
  dtoverlay=pps-gpio,pippin=18
  ```

## 4.2 Attach Chrony to a particular core
### Get chrony daemon (chronyd) process id (pid)
  ```bash
  ~$ pidof chronyd
  924 923
  ```

### Check the cores running the process
  ```bash
  ~$ sudo taskset -pc 923
  pid 923's current affinity list: 0-3
  ```
  ```bash
  ~$ sudo taskset -pc 924
  pid 924's current affinity list: 0-3
  ```

### Pin process to a particular core
  ```bash
  ~$ sudo taskset -pc 0 923
  pid 923's current affinity list: 0-3
  pid 923's new affinity list: 0
  ```
  ```bash
  ~$ sudo taskset -pc 0 924
  pid 924's current affinity list: 0-3
  pid 924's new affinity list: 0
  ```

**You can add the following script at the end of you .bashrc to check and assign core when opening a ssh session**
```text
prt_info "Check and Assign Chrony Core affinity"
pids=($(pidof chronyd))
printf "%s\n" "${pids[@]}"
start_core=0

for i in "${!pids[@]}"; do
    pid="${pids[$i]}"
    core=$start_core

    # Get current affinity (just the number list)
    current_affinity=$(sudo taskset -pc "$pid" 2>/dev/null | awk -F': ' '{print $2}')

    # If affinity is already correct, skip
    if [[ "$current_affinity" == "$core" ]]; then
        echo "Chrony already affected to core 0"
        continue
    fi

    # Otherwise assign it
    sudo taskset -pc "$core" "$pid"
done
  ```
---

# 5. Install system

---

## 5.1. Add device tree overlays for GPIO
- [In Module folder](#1-repository-structure)

### Build and load e2e_module.dts
This configures your gpio descriptor used in C module

  ```bash
  dtc -@ -I dts -O dtb -o e2e_module.dtbo e2e_module.dts
  sudo cp e2e_module.dtbo /boot/firmware/overlays
  ```
  Note:
  ```bash
  sudo rm /boot/firmware/overlays/e2e_module.dtbo # Prevents overlay load at boot time 
  ```

### Add overlay to boot config file
 - Open boot config file
  ```bash
  sudo nano /boot/firmware/config.txt
  ```
 - Add
  ````text
  #[all]
  ...
  # Module Overlay
  dtoverlay=e2e_module
  ````

### Reboot the device to apply changes
  ```bash
  sudo reboot
  ```

## 5.2. Build and load kernel module
- [In Module folder](#1-repository-structure)
### Build module
  ```bash
  make clean && make
  ```
### load module into kernel
  ```bash
  sudo insmod e2e_module.ko
  ```
### Check if module has been loaded
  ```bash
  $ sudo lsmod | grep e2e_module
  e2e_module             12288  0
  ```

### Check for error in kernel ring buffer
  ```bash
  $ sudo dmesg | tail -n 50
  ```

### Check if the GPIO is used by your module
  ```bash
  sudo gpioinfo
  ```

  ![alt text](images/gpioinfo.png)
  
  - **DO NOT REBOOT OR MODULE WILL UNLOADED**
  - Unload manually
    ```bash
    sudo rmmod e2e_module
    ```

### Launch bash script
- You can Add the following function to your .bashrc to build and run module automatically
```bash
  MODULE_FOLDER="PATH TO MODULE FOLDER"

  launch(){
      module="e2e_module"

      trap 'echo "Stopping live view..."' INT

      echo -e "Search existing module\n"
      loaded=$(lsmod | awk -v mod="$module" '$1 == mod {print $1}')
      echo $loaded
      if [ "$loaded" = "$module" ]; then
          echo "Module already loaded"
      else
          echo "Module is not loaded"
          echo -e "Building Module\n"
          cd $MODULE_FOLDER
          make clean && make
          cd -
          echo "Done"
          echo -e "Loading Module\n"
          sudo insmod "$MODULE_FOLDER/$module.ko"
          sudo dmesg | tail -50
          sudo dmesg -C
          echo "Done"
      fi
      echo -e "Start Monitoring\n"
      sudo dmesg --follow
      echo "Exit Monitoring"
      echo -e "Unloading\n"
      sudo rmmod "$module"
      echo "Done"
      echo -e "Saving data\n"
      i=0
      name="e2e_"$1"_${i}.txt"
      while [ -e "$MODULE_FOLDER/test_results/$name" ]; do
          ((i++))
          name="e2e_"$1"_${i}.txt"
      done
      sudo dmesg > "$MODULE_FOLDER/test_results/$name"
      echo "Data saved at $MODULE_FOLDER/test_results/$name"
      echo -e "Clearing LOGS\n"
      sudo dmesg -C
      echo "Done"
  }
```
- On use the script will automatically build and load the module then start monitoring in real-time
- Once you reach the wanted number of iterations, press CTRL+C to stop the process, the script will save logs into a text files then unload the module.
- **The script expect an argument for text files names use "station" and "vehicle" to have name format used in python script (e2e_extract.py) for data extraction**

## 5.3. Modify interrupt trigger
### Update trigger type in e2e_module.dts
  > interrupts = <16 2>;

  > interrupts = <gpio_num trigger_type>;

  #### Trigger Types
  <table>
    <tr>
      <th style="border-right:1px solid #888;">Number</th>
      <th>Interrupt Triggers</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">0 -> 0b0000</td>
      <td>Undefined</td>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">1 -> 0b0001</td>
      <td>Rising Edge</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">2 -> 0b0010</td>
      <td>Falling Edge</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">4 -> 0b0100</td>
      <td>Level High</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">8 -> 0b1000</td>
      <td>Level Low</td>
    </tr>
    <tr>
    <tr>
    <td colspan="2">
      Any number from 1 to 15 is valid <br>
      15 → 0b1111 meaning all triggers are selected
    </td>
  </tr>
  </tr>
  </table>

### Update trigger type in C module

  ```c
  ret = devm_request_irq(&pdev->dev, irq_number_pt, gpio_irq_handler_pt,
                           IRQF_TRIGGER_FALLING, "gpio_irq_handler_pt", NULL);
  ```
  #### Trigger FLags
  <table>
    <tr>
      <th style="border-right:1px solid #888;">Trigger</th>
      <th>FLAG</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">Rising Edge</td>
      <td>IRQF_TRIGGER_RISING</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">Falling Edge</td>
      <td>IRQF_TRIGGER_FALLING</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">Level High</td>
      <td>IRQF_TRIGGER_HIGH</th>
    </tr>
    <tr>
      <td style="border-right:1px solid #888;">Level Low</td>
      <td>IRQF_TRIGGER_LOW</th>
    </tr>
  </table>

  ### For multiple trigger use the notation "FLAG1 | FLAG2 | ..." 
  ```c
  ret = devm_request_irq(&pdev->dev, irq_number_pt, gpio_irq_handler_pt,
                           IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "gpio_irq_handler_pt", NULL);
  ```

### [Rebuild]5. Install system

**Note: To modify GPIO used as interrupt, in e2e_module.dts:**

- interrupts = <gpio_num trigger_type>;
- gpio-irq-gpios = <&gpio gpio_num trigger_type>;
- if **GPIO_16_IRQ:** is modified in module, it must be modifed everywhere else (cpp apps)

## 5.4. Read interrupt events logs
### Access the ring buffer
- Timestamps are logged into the ring buffer
  ```bash
  sudo dmesg
  ```

- Monitor in real-time
  ```bash
  sudo dmesg --follow
  ```

### Clear ring buffer 
- Must be done in between experiments to prevent mixing old ones with new ones
  ```bash
  sudo dmesg -C
  ```

# 6. Run synchronisation test application

---
- [In Module folder](#1-repository-structure)
- A module containing only the gpio irq logic is used for syncrhonisation. It is available in Sync folder you can follow previous steps to [build it]5. Install system
- Follow module and device tree build steps

- [In App folder](#1-repository-structure)

The full application is separated into sync_gps_client and sync_gps_server, each must be launched in one of the RPI
- **RPI-Client** = Raspberry Pi launching sync_gps_client.cpp
- **RPI-Server** = Raspberry Pi launching sync_gps_server.cpp

## 6.1. Hardware

- Connect RPI-Client GPIO-17 to its own GPIO-16 and RPI-Server GPIO-16 each via a 1kΩ resistor.
- Connect RPI-Client and RPI-Server GND pins.

![alt text](images/sync_schematic.png)

## 6.2. Client side
### Build and run 
  ```bash
  g++ -o sync_gps_client sync_gps_client.cpp -lgpiod
  sudo taskset -c 3 chrt -f 99 ./sync_gps_client
  ```
### Modify ip address to RPI-Server
 - Enter the correc IP address of you RPI-Server
  ```c
  // Compare timestamps and register store them
  int checkSynchronisation(int &n_meas, double &trigger)
  {
    // Extract local timestamps
    std::string response = getLatestDmesgLine();
    double time_Pi_two = extractTimestamp(response);

    // Ask other Pi for timestamps
    std::string dmesgLine = requestDmesgFromPi2("XX.XX.XX.XX"); // Change Value here
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
  ```

### Modify the number of iterations and toggle time

  ```c
  #define ITERATIONS 1800 // Adjust to number of iteration wanted
  #define SLEEP_S 0 // Adjust the seconds component for sleep time
  #define SLEEP_NS 500000000 // Adjust the nanoseconds
  // The system will sleep for Time = Seconds + Nanoseconds
  ```

### Modify Result file location
 - Enter your result folder here
  ```c
  // Export results into a csv
  int duration = static_cast<int>(meas_time);
  std::string path = "path/to/results";
  std::string baseFilename = path + "synchronisation_test_gps_" + 
                              std::to_string(duration) + "s";
  ```

## 6.3. Server side
### Build and run 
  ```bash
  g++ -o sync_gps_server sync_gps_server.cpp
  sudo taskset -c 3 chrt -f 99 ./sync_gps_server
  ```
## 6.4. Ouput data
Provide a csv containing
- synchronization offset in nanoseconds and absolute milliseconds
- offset jitter in nanoseconds and absolute value milliseconds
- GPIO trigger time (set to LOW) in nanoseconds and converted to seconds starting from 0

## 6.5. Notes
 - **sudo taskset -c 3 chrt -f 99** launch programs at maximum priority on core 3. It is needed for the GPIO to be set as fast as possible
 - **sync_gps_server** must be run before **sync_gps_client**
 - [WinSCP](https://winscp.net/eng/download.php) was used to transfer files between computer and RPI devices

---

# 7. About the latencies

---

- [Build and load dts and kernel module](#5-install-system)
- Connect the gyroscopes, GPS module and phototranistor following this schematic.

![alt text](images/schematic_sensor.png)

- **The phototransistor schematic allows to switch between 3.3V to 0V when light is detected. The resistor value may need to be tweeked depending on your conditions**
- start launch.sh on each side
```bash
# On remote station
launch station
# On vehicle 
launch vehicle
```

---

# 8. Summary

---

This repository provides:

- Measurement method for Motion-to-Motion, Glass-to-Glass and End-to-End
- A custom Linux kernel module including movement detection and phototransistor interrupt 
- Chrony-based time synchronization with GPS PPS  
- Real-time optimized client/server applications  
- Results export tools  

Ideal for:

- distributed time sync research  
- V2X timing systems  
- embedded real-time applications  
- latency and jitter studies  

---

# Contributions

---

Issues and pull requests are welcome!
