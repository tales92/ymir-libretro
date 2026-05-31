// =============================================================================
// Estado Global — mudança de nome para semântica mais clara
// =============================================================================
static bool g_system_reset_pending = true; // true = Reset+BRAM pendentes para o próximo frame

// =============================================================================
// retro_init — sem mudança de lógica, apenas nome da flag
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
        g_system_reset_pending = true;
        g_prev_width  = 0;
        g_prev_height = 0;

    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir] ERRO NO INIT: %s\n", e.what());
    }
}

// =============================================================================
// retro_deinit
// =============================================================================
extern "C" void retro_deinit(void) {
    g_pad1 = nullptr;
    g_saturn.reset();
    g_audio_buffer.clear();
    g_bram_mirror.fill(0x00);
    g_bram_path.clear();
    g_system_reset_pending = true;
}

// =============================================================================
// retro_load_game — CORREÇÃO: reseta flag e invalida pad
// =============================================================================
extern "C" bool retro_load_game(const struct retro_game_info* game) {
    try {
        if (!game || !game->path) return false;

        // CORREÇÃO 2: Garante que o bloco de init execute mesmo em recargas
        // de jogo sem passar por retro_deinit/retro_init.
        g_system_reset_pending = true;
        g_pad1 = nullptr;

        const char* save_dir = nullptr;
        if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
            std::error_code ec;
            std::filesystem::current_path(save_dir, ec);
            if (!ec) {
                g_bram_path = std::filesystem::path(save_dir) / "ymir_bram.bin";
            }
        }

        struct retro_controller_description controllers[] = {
            { "Controle Saturn Padrao", RETRO_DEVICE_JOYPAD }
        };
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
        if (!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir)
            return false;

        constexpr size_t kIPLSize = 524288;
        std::vector<uint8_t> iplData(kIPLSize);
        std::filesystem::path iplPath =
            std::filesystem::path(system_dir) / "saturn_bios.bin";
        std::ifstream iplFile(iplPath, std::ios::binary);

        if (!iplFile.is_open() ||
            !iplFile.read(reinterpret_cast<char*>(iplData.data()), kIPLSize))
            return false;

        g_saturn->LoadIPL(std::span<uint8_t, kIPLSize>(iplData.data(), kIPLSize));

        ymir::media::Disc disc;
        if (!ymir::media::LoadDisc(game->path, disc, false, nullptr))
            return false;

        g_saturn->LoadDisc(std::move(disc));
        g_saturn->UsePreferredRegion();
        g_saturn->CloseTray();
        // Reset e ConnectControlPad diferidos para o primeiro retro_run()

        if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "[Ymir] Disco carregado. Reset pendente para o primeiro frame.\n");

        return true;

    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir] ERRO NO LOAD_GAME: %s\n", e.what());
        return false;
    }
}

// =============================================================================
// retro_run — sem mudança de lógica, apenas nome da flag
// =============================================================================
extern "C" void retro_run(void) {
    try {
        if (g_system_reset_pending) {
            bram_push_to_ymir();
            g_saturn->Reset(true);
            g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();
            g_system_reset_pending = false;

            if (log_cb)
                log_cb(RETRO_LOG_INFO,
                       "[Ymir] Reset executado no primeiro frame. "
                       "BRAM injetada. Controle conectado.\n");
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
                    log_cb(RETRO_LOG_WARN,
                           "[Ymir] Aviso ignorado no RunFrame: %s\n", e.what());
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
                video_cb(g_video_fb, g_video_w, g_video_h,
                         g_video_w * sizeof(uint32_t));
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

// =============================================================================
// retro_reset — CORREÇÃO: ordem Reset → push (não push → Reset)
// =============================================================================
extern "C" void retro_reset(void) {
    if (!g_saturn) return;

    bram_pull_from_ymir();   // 1. salva estado atual no arquivo
    g_saturn->Reset(true);   // 2. Reset limpa subsistema de memória
    bram_push_to_ymir();     // 3. reinjeta BRAM no sistema já estável
    g_pad1 = g_saturn->SMPC.GetPeripheralPort1().ConnectControlPad(); // 4. reconecta pad
}
