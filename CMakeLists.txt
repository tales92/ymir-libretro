# =============================================================================
# Ymir Libretro Core - CMake Build System
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(Ymir_Libretro CXX)
# --- TRUQUE PARA IGNORAR O COMANDO DE VISUAL STUDIO DO YMIR ---
macro(vs_set_filters)
endmacro()
# --------------------------------------------------------------
# -----------------------------------------------------------------------------
# 1. Configurações Iniciais do Ymir (Devem vir ANTES do add_subdirectory)
# -----------------------------------------------------------------------------
# Forçamos a compilação estática das dependências do Ymir. O core Libretro
# não pode depender de DLLs/SOs externos de tempo de execução (como fmt, CHD, etc).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static build for ymir-core" FORCE)

# Desabilitamos logs de debug e asserções para máxima performance no RetroArch
set(Ymir_ENABLE_DEVLOG OFF CACHE BOOL "Disable dev logging" FORCE)
set(Ymir_ENABLE_DEV_ASSERTIONS OFF CACHE BOOL "Disable dev assertions" FORCE)
set(Ymir_DEV_BUILD OFF CACHE BOOL "Disable dev build mode" FORCE)

# Desabilitamos AVX2. Cores Libretro devem ser altamente portáteis.
# Usuários que querem OTIMIZAÇÕES LOCAIS podem ligar isso manualmente.
set(Ymir_AVX2 OFF CACHE BOOL "Disable AVX2 for maximum compatibility" FORCE)

# -----------------------------------------------------------------------------
# 2. Incluir o Motor Ymir
# -----------------------------------------------------------------------------
# Assumindo que o código original do Ymir está em libs/ymir-core.
# Ajuste este caminho conforme a estrutura da sua pasta.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/ymir-core)

# -----------------------------------------------------------------------------
# 3. Criar o Target do Core Libretro
# -----------------------------------------------------------------------------
# O RetroArch exige um módulo dinâmico (.so, .dll, .dylib)
add_library(ymir_libretro SHARED
    libretro.cpp
)

# -----------------------------------------------------------------------------
# 4. Propriedades do Target
# -----------------------------------------------------------------------------
# Padroniza o nome do arquivo de saída para 'ymir_libretro' (sem o 'lib' padrão no Linux)
set_target_properties(ymir_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ymir_libretro"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# -----------------------------------------------------------------------------
# 5. Includes e Definições
# -----------------------------------------------------------------------------
# Inclui o diretório onde está o libretro.h (assumindo que esteja na mesma pasta)
target_include_directories(ymir_libretro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Garante que a API do Libretro será exportada corretamente (especialmente no Windows/MSVC)
target_compile_definitions(ymir_libretro PRIVATE LIBRETRO_EXPORTS)

# -----------------------------------------------------------------------------
# 6. Linkagem
# -----------------------------------------------------------------------------
# Linka o motor do Ymir estaticamente no nosso core.
# Como ymir-core é compilado como STATIC (definido acima), o CMake vai embutir
# todo o código do Ymir, fmt, chdr, xxhash, etc., dentro do nosso arquivo final.
target_link_libraries(ymir_libretro PRIVATE ymir-core)

# -----------------------------------------------------------------------------
# 7. Ajustes Específicos por Plataforma
# -----------------------------------------------------------------------------
if(MSVC)
    # No Windows com MSVC, precisamos forçar a exportação dos símbolos do Libretro
    # e garantir que não haja dependências de DLLs do C++ runtime.
    target_compile_options(ymir_libretro PRIVATE /MT) # Usa o CRT estático (Multi-threaded)
    target_link_options(ymir_libretro PRIVATE 
        /DEF:${CMAKE_CURRENT_SOURCE_DIR}/libretro.def # Arquivo .def padrão do Libretro (opcional, mas seguro)
    )
else()
    # No Linux/macOS, esconde todos os símbolos por padrão, exceto os do Libretro (retro_*)
    # Isso evita conflitos com o frontend ou outros cores carregados na memória.
    target_compile_options(ymir_libretro PRIVATE -fvisibility=hidden)
    
    # No macOS, precisamos linkar contra o framework CoreFoundation (exigido por std::filesystem)
    if(APPLE)
        target_link_libraries(ymir_libretro PRIVATE "-framework CoreFoundation")
    endif()
endif()

# -----------------------------------------------------------------------------
# 8. Pós-build (Opcional)
# -----------------------------------------------------------------------------
# Copia o core automaticamente para a pasta de cores do RetroArch após compilar
# Descomente e ajuste o caminho conforme o seu sistema
# add_custom_command(TARGET ymir_libretro POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy 
#     $<TARGET_FILE:ymir_libretro> 
#     "C:/RetroArch-Win64/cores/"
# )
