WEBSOCKET_HOST         = "0.0.0.0"
WEBSOCKET_PORT         = 8765

AUDIO_SAMPLE_RATE      = 16000
AUDIO_CHANNELS         = 1
AUDIO_BIT_DEPTH        = 16

WHISPER_MODEL          = "medium"
WHISPER_COMPUTE_TYPE   = "int8"      # Optimal für CPU

PYANNOTE_TOKEN         = "hf_..."    # HuggingFace Token eintragen
PYANNOTE_MIN_SPEAKERS  = 1
PYANNOTE_MAX_SPEAKERS  = None        # Flexibel/automatisch

OLLAMA_URL             = "http://localhost:11434/api/generate"
OLLAMA_MODEL           = "mistral"   # oder gemma3:4b

TELEGRAM_BOT_TOKEN     = "..."       # Bot-Token eintragen
TELEGRAM_CHAT_ID       = "..."       # Chat-ID eintragen

OUTPUT_DIR             = "./output"
