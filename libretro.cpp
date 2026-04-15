// =============================================================================
// Ymir Core - Libretro Interface (Sega Saturn Emulator)
// =============================================================================
// Versão Final Completa: CPU, VDP (Vídeo Dinâmico), SCSP (Áudio Push), 
// LoadDisc (Mídia) e SMPC (Input Active-Low) totalmente integrados.
// =============================================================================

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
static ymir::peripheral::ControlPad* g_pad1 = nullptr; // Ponteiro direto para o Jogador 1

// --- Vídeo State ---
static const uint32* g_video_fb = nullptr;
static uint32_t g_video_w = 0;
static uint32_t g_video_h = 0;
static uint32_t g_prev_width = 0;
static uint32_t g_prev_height = 0;

// --- Áudio State ---
static std::vector<int16_t> g_audio_buffer;

// Callbacks Estáticos do Ambiente Libretro
static retro_environment_t env_cb           = NULL;
static retro_video_refresh_t video_cb       = NULL;
static retro_audio_sample_batch_t audio_cb  = NULL;
static retro_input_poll_t input_poll_cb     = NULL;
static retro_input_state_t input_state_cb   = NULL;

// =============================================================================
// Callbacks de Push do Ymir (Chamados internamente durante RunFrame)
// =============================================================================

// O VDP2 chama esta função quando termina de compor um frame XRGB8888
static void ymir_video_frame_complete(uint32 *fb, uint32 width, uint32 height) {
    g_video_fb = fb;
    g_video_w = width;
    g_video_h = height;
}

// O SCSP chama esta função para cada amostra estéreo gerada
// O SCSP chama esta função para cada amostra estéreo gerada
static void ymir_audio_output_callback(sint16 left, sint16 right) {
    g_audio_buffer.push_back(left);
    g_audio_buffer.push_back(right);
}

// =============================================================================
// Funções de Mapeamento de Input
// =============================================================================
static void update_ymir_input() {
    if (!g_saturn || !g_pad1 || !input_state_cb) return;

    // Acessa o report do controle diretamente (Requer GetReport() adicionado por você)
    auto& report = g_pad1->GetReport();

    // No protocolo do Saturn, os botões são ACTIVE-LOW:
    // 0 = Apertado, 1 = Solto. Por isso usamos o operador "!" para inverter 
    // o estado vindo do RetroArch (onde 1 = Apertado).

    // D-Pad
    report.up     = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    report.down   = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    report.left   = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    report.right  = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
    
    // Start
    report.start  = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    
    // Mão direita (Layout padrão do Saturn no RetroArch)
    report.a      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    report.b      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    report.c      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
    report.x      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
    report.y      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    report.z      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
    
    // Ombros (Triggers)
    report.l      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
    report.r      = !input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
}

// =============================================================================
// Funções de Registro de Callbacks (Obrigatórias)
// =============================================================================

extern "C" void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
extern "C" void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
extern "C" void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
extern "C" void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
extern "C" void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
extern "C" void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// =============================================================================
// Funções Informativas (Obrigatórias)
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
// Funções de Ciclo de Vida (Obrigatórias)
// =============================================================================

extern "C" void retro_init(void) {
    g_saturn = std::make_unique<ymir::Saturn>();
    
    if (g_saturn) {
        // 1. Configurar o renderizador de Vídeo (Software, síncrono)
        auto* swRenderer = g_saturn->VDP.UseSoftwareRenderer();
        if (swRenderer) {
            // Desabilitar threads internas do Ymir para evitar race conditions no Libretro
            swRenderer->EnableThreadedVDP1(false);
            swRenderer->EnableThreadedVDP2(false);
        }
        g_saturn->VDP.SetSoftwareRenderCallback(ymir_video_frame_complete);

        // 2. Configurar o callback de Áudio
        g_saturn->SCSP.SetSampleCallback(ymir_audio_output_callback);

        // 3. Conectar o Control Pad na Porta 1 do SMPC
        g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
    }
    
    g_prev_width = 0;
    g_prev_height = 0;
}

extern "C" void retro_deinit(void) {
    g_pad1 = nullptr; // Será destruído junto com o g_saturn
    g_saturn.reset();
    g_audio_buffer.clear();
}

extern "C" bool retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->path) return false;

    // 1. Formato de pixel do frontend
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        printf("[Ymir Libretro] Aviso: Frontend não suporta XRGB8888.\n");
    }

    // 2. Carregar a BIOS (IPL ROM)
    const char *system_dir = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
        constexpr size_t kIPLSize = 524288; 
        std::vector<uint8_t> iplData(kIPLSize);
        std::filesystem::path iplPath = std::filesystem::path(system_dir) / "saturn_bios.bin";
        std::ifstream iplFile(iplPath, std::ios::binary);
        
        if (iplFile.is_open() && iplFile.read(reinterpret_cast<char*>(iplData.data()), kIPLSize)) {
            g_saturn->LoadIPL(std::span<uint8_t, kIPLSize>(iplData.data(), kIPLSize));
        } else {
            printf("[Ymir Libretro] Erro fatal: BIOS do Saturn não encontrada em %s\n", iplPath.string().c_str());
            return false; 
        }
    } else {
        printf("[Ymir Libretro] Erro fatal: Diretório de sistema não definido no RetroArch.\n");
        return false;
    }

    // 3. Carregar a imagem do Disco
    printf("[Ymir Libretro] Carregando disco: %s\n", game->path);
    
    ymir::media::Disc disc;
    // preloadToRAM = false para economizar memória, cbMsg = nullptr (sem UI de log)
    bool loaded = ymir::media::LoadDisc(game->path, disc, false, nullptr);
    
    if (!loaded) {
        printf("[Ymir Libretro] Falha ao analisar/abrir o arquivo de disco!\n");
        return false;
    }

    // 4. Injeta o disco no drive e auto-detecta a região
    g_saturn->LoadDisc(std::move(disc));
    // TODO: Ativar detecção automática de região assim que a API do Ymir para AreaCode for mapeada
    // g_saturn->AutodetectRegion(g_saturn->GetDisc().header.GetAreaCodes());

    // 5. Hard Reset para dar boot no disco com a BIOS
    g_saturn->Reset(true); 

    return true;
}

extern "C" void retro_unload_game(void) {
    if (g_saturn) g_saturn->EjectDisc();
}

// =============================================================================
// O Loop Principal (Obrigatório)
// =============================================================================

extern "C" void retro_run(void) {
    // 1. Input
    if (input_poll_cb) input_poll_cb();
    update_ymir_input();

    // 2. Limpar os buffers de interceptação antes do frame
    g_video_fb = nullptr;
    g_audio_buffer.clear();

    // 3. Emulação (O coração do emulador)
    if (g_saturn) {
        g_saturn->RunFrame();
    }

    // 4. Enviar Vídeo para o RetroArch
    if (video_cb) {
        if (g_video_fb != nullptr) {
            // Atualiza a geometria dinamicamente se a resolução interna do VDP mudou
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
            // Se o callback de vídeo não disparou neste frame, envia frame vazio
            video_cb(NULL, 320, 240, 320 * sizeof(uint32_t));
        }
    }

    // 5. Enviar Áudio para o RetroArch
    if (audio_cb && !g_audio_buffer.empty()) {
        // audio_cb espera o número de FRAMES (pares L+R), e não samples individuais
        size_t num_frames = g_audio_buffer.size() / 2;
        audio_cb(g_audio_buffer.data(), num_frames);
    }
}

// =============================================================================
// Funções de Suporte e Stubs Obrigatórios
// =============================================================================

extern "C" void retro_reset(void) { 
    if (g_saturn) g_saturn->Reset(true); 
}

extern "C" size_t retro_serialize_size(void) { return 0; }
extern "C" bool retro_serialize(void *data, size_t size) { return false; }
extern "C" bool retro_unserialize(const void *data, size_t size) { return false; }
extern "C" void retro_cheat_reset(void) {}
extern "C" void retro_cheat_set(unsigned index, bool enabled, const char *code) {}

extern "C" unsigned retro_get_region(void) { 
    // Futuro: checar g_saturn->GetVideoStandard() para alternar dinamicamente entre NTSC e PAL
    return RETRO_REGION_NTSC; 
}

extern "C" bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { 
    return false; 
}

extern "C" void *retro_get_memory_data(unsigned id) { return NULL; }
extern "C" size_t retro_get_memory_size(unsigned id) { return 0; }
