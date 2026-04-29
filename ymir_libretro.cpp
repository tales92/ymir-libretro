// =============================================================================
// Ymir Core - Libretro Interface (Sega Saturn Emulator)
// =============================================================================
// Versão Final Completa: CPU, VDP (Vídeo Dinâmico), SCSP (Áudio Push), 
// LoadDisc (Mídia) e SMPC (Input Active-Low) totalmente integrados.
// BLINDADO: Com blocos try/catch e Log Nativo do RetroArch (log_cb).
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
#include <algorithm>

// =============================================================================
// Inclusões do Motor Ymir
// =============================================================================
#include <ymir/sys/saturn.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>
#include <ymir/media/disc.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_port.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_impl_control_pad.hpp>

// =============================================================================
// Estado Global do Core
// =============================================================================
static std::unique_ptr<ymir::Saturn> g_saturn = nullptr;
static ymir::peripheral::ControlPad* g_pad1 = nullptr;

static const uint32* g_video_fb = nullptr;
static uint32_t g_video_w = 0;
static uint32_t g_video_h = 0;
static uint32_t g_prev_width = 0;
static uint32_t g_prev_height = 0;

static std::vector<int16_t> g_audio_buffer;

// Callbacks Estáticos do Ambiente Libretro
static retro_environment_t env_cb           = NULL;
static retro_video_refresh_t video_cb       = NULL;
static retro_audio_sample_batch_t audio_cb  = NULL;
static retro_input_poll_t input_poll_cb     = NULL;
static retro_input_state_t input_state_cb   = NULL;

// O Rádio Comunicador Oficial do RetroArch
static retro_log_printf_t log_cb = NULL;

// =============================================================================
// Callbacks de Push do Ymir
// =============================================================================

static void ymir_video_frame_complete(uint32 *fb, uint32 width, uint32 height, void* /*userdata*/) {
    g_video_fb = fb;
    g_video_w = width;
    g_video_h = height;
}

static void ymir_audio_output_callback(sint16 left, sint16 right, void* /*userdata*/) {
    g_audio_buffer.push_back(left);
    g_audio_buffer.push_back(right);
}

// =============================================================================
// Funções de Mapeamento de Input
// =============================================================================
static void update_ymir_input() {
    if (!g_saturn || !g_pad1 || !input_state_cb) return;

    auto& report = g_pad1->GetReport();
    auto current_buttons = ymir::peripheral::Button::All;

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
    
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))    current_buttons &= ~ymir::peripheral::Button::L;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))    current_buttons &= ~ymir::peripheral::Button::R;

    report.buttons = current_buttons;
}

// =============================================================================
// Funções de Registro de Callbacks (Obrigatórias)
// =============================================================================

extern "C" void retro_set_environment(retro_environment_t cb) { 
    env_cb = cb; 
    
    // Capturando a função de Log do RetroArch
    struct retro_log_callback logging;
    if (env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    }
}
extern "C" void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
extern "C" void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
extern "C" void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
extern "C" void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
extern "C" void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// =============================================================================
// Funções Informativas
// =============================================================================

extern "C" unsigned retro_api_version(void) { return RETRO_API_VERSION; }

extern "C" void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Ymir";
    info->library_version  = "0.1.0";
    info->valid_extensions = "ccd|chd|cue|iso|mds"; 
    info->need_fullpath    = true; 
    info->block_extract    = false;
}

extern "C" void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 320;
    info->geometry.base_height  = 240;
    info->geometry.max_width    = 704;
    info->geometry.max_height   = 512;
    info->geometry.aspect_ratio = 4.0 / 3.0;
    info->timing.fps            = 59.94;
    info->timing.sample_rate    = 44100.0;
}

// =============================================================================
// Funções de Ciclo de Vida
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
            g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
        }
        
        g_prev_width = 0;
        g_prev_height = 0;
        if (log_cb) log_cb(RETRO_LOG_INFO, "[Ymir Libretro] Nucleo inicializado com sucesso.\n");
    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] ERRO FATAL NO INIT: %s\n", e.what());
    }
}

extern "C" void retro_deinit(void) {
    g_pad1 = nullptr;
    g_saturn.reset();
    g_audio_buffer.clear();
}

extern "C" bool retro_load_game(const struct retro_game_info *game) {
    try {
        if (!game || !game->path) return false;

        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
            if (log_cb) log_cb(RETRO_LOG_WARN, "[Ymir Libretro] Aviso: Frontend nao suporta XRGB8888.\n");
        }

        const char *system_dir = NULL;
        if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
            constexpr size_t kIPLSize = 524288; 
            std::vector<uint8_t> iplData(kIPLSize);
            std::filesystem::path iplPath = std::filesystem::path(system_dir) / "saturn_bios.bin";
            std::ifstream iplFile(iplPath, std::ios::binary);
            
            if (iplFile.is_open() && iplFile.read(reinterpret_cast<char*>(iplData.data()), kIPLSize)) {
                g_saturn->LoadIPL(std::span<uint8_t, kIPLSize>(iplData.data(), kIPLSize));
                if (log_cb) log_cb(RETRO_LOG_INFO, "[Ymir Libretro] BIOS carregada: %s\n", iplPath.string().c_str());
            } else {
                if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] Erro fatal: BIOS (saturn_bios.bin) nao encontrada em %s\n", iplPath.string().c_str());
                return false; 
            }
        } else {
            if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] Erro fatal: Diretorio de sistema (system_dir) nao definido no RetroArch.\n");
            return false;
        }

        if (log_cb) log_cb(RETRO_LOG_INFO, "[Ymir Libretro] Carregando disco: %s\n", game->path);
        
        ymir::media::Disc disc;
        bool loaded = ymir::media::LoadDisc(game->path, disc, false, nullptr);
        
        if (!loaded) {
            if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] Falha ao analisar/abrir o arquivo de disco: %s\n", game->path);
            return false;
        }

        g_saturn->LoadDisc(std::move(disc));
        g_saturn->Reset(true); 

        if (log_cb) log_cb(RETRO_LOG_INFO, "[Ymir Libretro] Jogo carregado e resetado com sucesso.\n");
        return true;
    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] EXCECAO C++ DURANTE O LOAD_GAME: %s\n", e.what());
        return false;
    }
}

extern "C" void retro_unload_game(void) {
    if (g_saturn) g_saturn->EjectDisc();
}

// =============================================================================
// O Loop Principal
// =============================================================================

extern "C" void retro_run(void) {
    try {
        if (input_poll_cb) input_poll_cb();
        update_ymir_input();

        g_video_fb = nullptr;
        g_audio_buffer.clear();

        if (g_saturn) {
            g_saturn->RunFrame();
        }

        if (video_cb) {
            if (g_video_fb != nullptr) {
                if (g_video_w != g_prev_width || g_video_h != g_prev_height) {
                    struct retro_game_geometry geom;
                    geom.base_width   = g_video_w;
                    geom.base_height  = g_video_h;
                    geom.max_width    = 704;
                    geom.max_height   = 512;
                    geom.aspect_ratio = 4.0 / 3.0;
                    env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
                    g_prev_width = g_video_w;
                    g_prev_height = g_video_h;
                }
                unsigned pitch = g_video_w * sizeof(uint32_t);
                video_cb(g_video_fb, g_video_w, g_video_h, pitch);
            } else {
                video_cb(NULL, 320, 240, 320 * sizeof(uint32_t));
            }
        }

        if (audio_cb && !g_audio_buffer.empty()) {
            size_t num_frames = g_audio_buffer.size() / 2;
            audio_cb(g_audio_buffer.data(), num_frames);
        }
    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] ERRO FATAL DURANTE A EXECUCAO (RUN): %s\n", e.what());
    }
}

// =============================================================================
// Funções de Suporte
// =============================================================================

extern "C" void retro_reset(void) { if (g_saturn) g_saturn->Reset(true); }

// ---> ADICIONE ESTAS 3 LINHAS AQUI <---
extern "C" void retro_set_controller_port_device(unsigned port, unsigned device) {
    // Stub vazio obrigatório pelo RetroArch
}

extern "C" size_t retro_serialize_size(void) { return 0; }
extern "C" bool retro_serialize(void *data, size_t size) { return false; }
extern "C" bool retro_unserialize(const void *data, size_t size) { return false; }
extern "C" void retro_cheat_reset(void) {}
extern "C" void retro_cheat_set(unsigned index, bool enabled, const char *code) {}
extern "C" unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
extern "C" bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { return false; }
extern "C" void *retro_get_memory_data(unsigned id) { return NULL; }
extern "C" size_t retro_get_memory_size(unsigned id) { return 0; }
