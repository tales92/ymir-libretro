// =============================================================================
// Ymir Core - Libretro Interface (Sega Saturn Emulator)
// =============================================================================
// Versão Final: BRAM Formal implementada + Sincronização de Controles Corrigida.
// =============================================================================

#include <exception>
#include <libretro.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <memory>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <algorithm>
#include <span>

// =============================================================================
// Inclusões do Motor Ymir
// =============================================================================
#include <ymir/sys/saturn.hpp>
#include <ymir/sys/backup_ram_defs.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/media/disc.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_port.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_impl_control_pad.hpp>

// =============================================================================
// Estado Global do Core
// =============================================================================
static std::unique_ptr<ymir::Saturn> g_saturn = nullptr;
static ymir::peripheral::ControlPad* g_pad1   = nullptr;
static bool g_first_frame = true;

static const uint32* g_video_fb  = nullptr;
static uint32_t      g_video_w   = 0;
static uint32_t      g_video_h   = 0;
static uint32_t      g_prev_width  = 0;
static uint32_t      g_prev_height = 0;

static std::vector<int16_t> g_audio_buffer;

static constexpr size_t k_BRAM_Size = 32 * 1024; // 32.768 bytes
static std::array<uint8_t, k_BRAM_Size> g_bram_mirror{};
static std::filesystem::path g_bram_path; 

// Callbacks Estáticos do Ambiente Libretro
static retro_environment_t        env_cb         = nullptr;
static retro_video_refresh_t      video_cb       = nullptr;
static retro_audio_sample_batch_t audio_cb       = nullptr;
static retro_input_poll_t         input_poll_cb  = nullptr;
static retro_input_state_t        input_state_cb = nullptr;
static retro_log_printf_t         log_cb         = nullptr;

// =============================================================================
// Callbacks de Push do Ymir
// =============================================================================

static void ymir_video_frame_complete(uint32* fb, uint32 width, uint32 height, void*) {
    g_video_fb = fb;
    g_video_w  = width;
    g_video_h  = height;
}

static void ymir_audio_output_callback(sint16 left, sint16 right, void*) {
    g_audio_buffer.push_back(left);
    g_audio_buffer.push_back(right);
}

// =============================================================================
// Sincronização BRAM
// =============================================================================

static void bram_push_to_ymir() {
    if (!g_saturn || g_bram_path.empty()) return;

    {
        std::ofstream f(g_bram_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f.write(reinterpret_cast<const char*>(g_bram_mirror.data()), static_cast<std::streamsize>(k_BRAM_Size));
    }

    std::error_code ec;
    g_saturn->LoadInternalBackupMemoryImage(g_bram_path, false, ec);
    if (ec && log_cb) {
        log_cb(RETRO_LOG_WARN, "[Ymir] BRAM push falhou: %s\n", ec.message().c_str());
    }
}

static void bram_pull_from_ymir() {
    if (g_bram_path.empty()) return;

    std::ifstream f(g_bram_path, std::ios::binary);
    if (!f.is_open()) return;

    f.read(reinterpret_cast<char*>(g_bram_mirror.data()), static_cast<std::streamsize>(k_BRAM_Size));
}

// =============================================================================
// Mapeamento de Input
// =============================================================================
static void update_ymir_input() {
    if (!g_saturn || !g_pad1 || !input_state_cb) return;

    auto& report          = g_pad1->GetReport();
    auto  current_buttons = ymir::peripheral::Button::All; 

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    current_buttons &= ~ymir::peripheral::Button::Up;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  current_buttons &= ~ymir::peripheral::Button::Down;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  current_buttons &= ~ymir::peripheral::Button::Left;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) current_buttons &= ~ymir::peripheral::Button::Right;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) current_buttons &= ~ymir::peripheral::Button::Start;

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))     current_buttons &= ~ymir::peripheral::Button::A;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))     current_buttons &= ~ymir::peripheral::Button::B;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))     current_buttons &= ~ymir::peripheral::Button::C;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))     current_buttons &= ~ymir::peripheral::Button::X;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))     current_buttons &= ~ymir::peripheral::Button::Y;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))     current_buttons &= ~ymir::peripheral::Button::Z;

    report.buttons = current_buttons;
}

// =============================================================================
// Funções de Registro
// =============================================================================

extern "C" void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    struct retro_log_callback logging;
    if (env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
}
extern "C" void retro_set_video_refresh(retro_video_refresh_t cb)            { video_cb       = cb; }
extern "C" void retro_set_audio_sample(retro_audio_sample_t cb)               { (void)cb;           }
extern "C" void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)   { audio_cb       = cb; }
extern "C" void retro_set_input_poll(retro_input_poll_t cb)                   { input_poll_cb  = cb; }
extern "C" void retro_set_input_state(retro_input_state_t cb)                 { input_state_cb = cb; }

extern "C" unsigned retro_api_version(void) { return RETRO_API_VERSION; }

extern "C" void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Ymir";
    info->library_version  = "0.1.0";
    info->valid_extensions = "ccd|chd|cue|iso|mds";
    info->need_fullpath    = true;
    info->block_extract    = false;
}

extern "C" void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 320;
    info->geometry.base_height  = 240;
    info->geometry.max_width    = 704;
    info->geometry.max_height   = 512;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = 59.94;
    info->timing.sample_rate    = 44100.0;
}

// =============================================================================
// Ciclo de Vida
// =============================================================================

extern "C" void retro_init(void) {
    try {
        g_saturn = std::make_unique<ymir::Saturn>();

        if (g_saturn) {
            auto* swRenderer = g_saturn->VDP.UseSoftwareRenderer();
            if (swRenderer) {
                swRenderer->EnableThreadedVDP1(false);
                swRenderer->EnableThreadedVDP2(false);
            }
            g_saturn->VDP.SetSoftwareRenderCallback(ymir_video_frame_complete);
            g_saturn->SCSP.SetSampleCallback(ymir_audio_output_callback);
        }

        g_bram_mirror.fill(0x00);
        g_bram_path.clear();
        g_first_frame = true;
        g_prev_width  = 0;
        g_prev_height = 0;

    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir] ERRO NO INIT: %s\n", e.what());
    }
}

extern "C" void retro_deinit(void) {
    g_pad1 = nullptr;
    g_saturn.reset();
    g_audio_buffer.clear();
    g_bram_mirror.fill(0x00);
    g_bram_path.clear();
    g_first_frame = true;
}

extern "C" bool retro_load_game(const struct retro_game_info* game) {
    try {
        if (!game || !game->path) return false;

        const char* save_dir = nullptr;
        if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
            std::error_code ec;
            std::filesystem::current_path(save_dir, ec);
            if (!ec) {
                g_bram_path = std::filesystem::path(save_dir) / "ymir_bram.bin";
            }
        }

        struct retro_controller_description controllers[] = { { "Controle Saturn Padrao", RETRO_DEVICE_JOYPAD } };
        struct retro_controller_info ports[] = { { controllers, 1 }, { nullptr, 0 } };
        env_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

        struct retro_input_descriptor desc[] = {
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Cima"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Baixo"    },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Esquerda" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Direita"  },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z (Saturn)"     },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"          },
            { 0, 0, 0, 0, nullptr }
        };
        env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

        const char* system_dir = nullptr;
        if (!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir) return false;

        constexpr size_t kIPLSize = 524288;
        std::vector<uint8_t> iplData(kIPLSize);
        std::filesystem::path iplPath = std::filesystem::path(system_dir) / "saturn_bios.bin";
        std::ifstream iplFile(iplPath, std::ios::binary);

        if (!iplFile.is_open() || !iplFile.read(reinterpret_cast<char*>(iplData.data()), kIPLSize)) return false;

        g_saturn->LoadIPL(std::span<uint8_t, kIPLSize>(iplData.data(), kIPLSize));

        ymir::media::Disc disc;
        if (!ymir::media::LoadDisc(game->path, disc, false, nullptr)) return false;

        g_saturn->LoadDisc(std::move(disc));
        g_saturn->UsePreferredRegion();
        g_saturn->CloseTray();
        g_saturn->Reset(true); 

        // O controle E a memória serão conectados no PRIMEIRO FRAME do retro_run, 
        // para garantir que os dados do RetroArch chegaram e evitar a perda de conexão.

        return true;
    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir] ERRO NO LOAD_GAME: %s\n", e.what());
        return false;
    }
}

extern "C" void retro_unload_game(void) {
    bram_pull_from_ymir();
    if (g_saturn) g_saturn->EjectDisc();
}

// =============================================================================
// Loop Principal
// =============================================================================

extern "C" void retro_run(void) {
    try {
        if (g_first_frame) {
            // A BRAM é injetada de forma segura com os dados que o RetroArch enviou
            bram_push_to_ymir();
            // O controle é plugado LOGO EM SEGUIDA, blindado de qualquer reset interno
            g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
            g_first_frame = false;
        }

        if (input_poll_cb) input_poll_cb();
        update_ymir_input();

        g_video_fb = nullptr;
        g_audio_buffer.clear();

        if (g_saturn) {
            try {
                g_saturn->RunFrame();
            } catch (const std::exception& e) {
                static bool aviso_dado = false;
                if (!aviso_dado && log_cb) {
                    log_cb(RETRO_LOG_WARN, "[Ymir] Aviso ignorado no RunFrame: %s\n", e.what());
                    aviso_dado = true;
                }
            }
        }

        if (video_cb) {
            if (g_video_fb) {
                if (g_video_w != g_prev_width || g_video_h != g_prev_height) {
                    struct retro_game_geometry geom;
                    geom.base_width   = g_video_w;
                    geom.base_height  = g_video_h;
                    geom.max_width    = 704;
                    geom.max_height   = 512;
                    geom.aspect_ratio = 4.0f / 3.0f;
                    env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
                    g_prev_width  = g_video_w;
                    g_prev_height = g_video_h;
                }
                video_cb(g_video_fb, g_video_w, g_video_h, g_video_w * sizeof(uint32_t));
            } else {
                video_cb(nullptr, 320, 240, 320 * sizeof(uint32_t));
            }
        }

        if (audio_cb && !g_audio_buffer.empty())
            audio_cb(g_audio_buffer.data(), g_audio_buffer.size() / 2);

    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir] ERRO NO RUN: %s\n", e.what());
    }
}

extern "C" void retro_reset(void) {
    if (!g_saturn) return;
    bram_pull_from_ymir();
    g_saturn->Reset(true);
    bram_push_to_ymir();
    g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
}

// =============================================================================
// Stubs e BRAM Formal
// =============================================================================

extern "C" void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port; (void)device;
}
extern "C" void retro_cheat_reset(void) {}
extern "C" void retro_cheat_set(unsigned i, bool e, const char* c) {
    (void)i; (void)e; (void)c;
}
extern "C" unsigned retro_get_region(void)  { return RETRO_REGION_NTSC; }
extern "C" bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

extern "C" size_t retro_serialize_size(void)                        { return 0;     }
extern "C" bool   retro_serialize(void* data, size_t size)          { return false; }
extern "C" bool   retro_unserialize(const void* data, size_t size)  { return false; }

extern "C" void* retro_get_memory_data(unsigned id) {
    if (id != RETRO_MEMORY_SAVE_RAM) return nullptr;
    return g_bram_mirror.data();
}

extern "C" size_t retro_get_memory_size(unsigned id) {
    if (id != RETRO_MEMORY_SAVE_RAM) return 0;
    return k_BRAM_Size; 
}
