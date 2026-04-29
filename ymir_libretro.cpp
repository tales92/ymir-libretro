extern "C" bool retro_load_game(const struct retro_game_info *game) {
    try {
        if (!game || !game->path) return false;

        // 1. O DICIONÁRIO DE CONTROLES (ISSO LIBERA O MENU DO RETROARCH)
        struct retro_input_descriptor desc[] = {
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Cima" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Baixo" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Esquerda" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Direita" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Botao A (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Botao B (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Botao C (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Botao X (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Botao Y (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Botao Z (Saturn)" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Gatilho L" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Gatilho R" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
            { 0, 0, 0, 0, NULL }
        };
        if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

        // 2. Configuração de Vídeo
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
            if (log_cb) log_cb(RETRO_LOG_WARN, "[Ymir Libretro] Aviso: Frontend nao suporta XRGB8888.\n");
        }

        // 3. Carregamento da BIOS (Lembre-se de colocar a BIOS da região certa com este nome!)
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
                if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] Erro fatal: BIOS nao encontrada.\n");
                return false; 
            }
        } else {
            return false;
        }

        // 4. Carregamento do Disco
        if (log_cb) log_cb(RETRO_LOG_INFO, "[Ymir Libretro] Carregando disco: %s\n", game->path);
        
        ymir::media::Disc disc;
        bool loaded = ymir::media::LoadDisc(game->path, disc, false, nullptr);
        if (!loaded) return false;

        g_saturn->LoadDisc(std::move(disc));
        
        // 5. Ajustes finais do Console para garantir o boot
        g_saturn->UsePreferredRegion(); // Tenta alinhar a região interna do console
        g_saturn->CloseTray(); // Garante que a tampa do CD virtual está fechada
        g_saturn->Reset(true); 

        return true;
    } catch (const std::exception& e) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[Ymir Libretro] EXCECAO: %s\n", e.what());
        return false;
    }
}
