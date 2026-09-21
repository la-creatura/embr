#rm build -rd
#rm bin/linux/* -rd
#rm bin/windows/* -rd
cmake --preset linux-release
cmake --build --preset linux-release
cmake --preset windows-release
cmake --build --preset windows-release
cd ./bin/linux/
./embr