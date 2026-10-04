# writes the build directory's name into a file: cmake -DNAME=<name> -DOUT=<file> -P write_stamp.cmake
file(WRITE "${OUT}" "${NAME}\n")
