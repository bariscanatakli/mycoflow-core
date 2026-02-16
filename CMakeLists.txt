cmake_minimum_required(VERSION 3.10)

# Proje Adı ve Versiyonu
project(MycoFlow VERSION 0.1.0 LANGUAGES C)

# Derleme Seçenekleri
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

enable_testing()

# src klasörünü projeye dahil et
add_subdirectory(src)
