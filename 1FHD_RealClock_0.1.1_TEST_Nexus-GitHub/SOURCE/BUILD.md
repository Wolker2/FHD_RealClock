# Сборка

Для нативной части нужен MinGW-w64 x86. Из каталога FHD_RealClock_SOURCE выполните bash build.sh /absolute/path/FHDRealClock.dll.

Для Main.dat нужен Python 3 и [rangers-utils](https://github.com/murgesku/rangers-utils) в PYTHONPATH. Из FHD_RealClock_SOURCE выполните python3 build_dat.py /absolute/path/Main.dat.

Тест tests/verify_pe.py проверяет собранную DLL, используя pefile и unicorn. Аргументы по порядку — DLL, EXE Steam/GOG, EXE предрелиза, выходной JSON геометрии. Тест tests/verify_dat.py принимает Main.dat и независимый декодер dat_codec.py из исходников FHD Navigator 0.2.1.
