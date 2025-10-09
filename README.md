git submodule add -b stable https://github.com/pybind/pybind11 extern/pybind11
git submodule update --init

rm -rf build; cmake -S . -B build; cmake --build build
pip wheel .