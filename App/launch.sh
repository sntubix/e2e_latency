#!/bin/bash

MODULE_FOLDER="PATH TO MODULE FOLDER"

prt_info()
{
        info="   "${1^^}""
        echo ""
        echo "**********************************************************************************************************************"
        echo $info
        echo "**********************************************************************************************************************"
}


module="e2e_module"

trap 'echo "Stopping live view..."' INT

prt_info "Search existing module"
loaded=$(lsmod | awk -v mod="$module" '$1 == mod {print $1}')
echo $loaded
if [ "$loaded" = "$module" ]; then
    echo "Module already loaded"
else
    echo "Module is not loaded"
    prt_info "Building Module"
    cd $MODULE_FOLDER
    make clean && make
    cd -
    echo "Done"
    prt_info "Loading Module"
    sudo insmod "$MODULE_FOLDER/$module.ko"
    sudo dmesg | tail -50
    sudo dmesg -C
    echo "Done"
fi
prt_info "Start Monitoring"
sudo dmesg --follow
echo "Exit Monitoring"
prt_info "Unloading"
sudo rmmod "$module"
echo "Done"
prt_info "Saving data"
i=0
name="e2e_"$1"_${i}.txt"
while [ -e "$MODULE_FOLDER/test_results/$name" ]; do
    ((i++))
    name="e2e_"$1"_${i}.txt"
done
sudo dmesg > "$MODULE_FOLDER/test_results/$name"
echo "Data saved at $MODULE_FOLDER/test_results/$name"
prt_info "Clearing LOGS"
sudo dmesg -C
echo "Done"
