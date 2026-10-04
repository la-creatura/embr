#rm build -rd
#rm bin/linux/* -rd
#rm bin/windows/* -rd
cmake --preset linux-vm-release
cmake --build --preset linux-vm-release --parallel 6
cmake --preset windows-vm-release
cmake --build --preset windows-vm-release --parallel 6
cd ./bin/linux/
./embr
