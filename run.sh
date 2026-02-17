#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/whispercpp"
MODELS="$SCRIPT_DIR/models"

export LD_LIBRARY_PATH="$BIN"

# ── State ──
MODE="normal"
SEL=0
FILTER=""
ITEMS=()
TOTAL=0

# ── Terminal ──
cleanup() { printf '\e[?25h\e[0m'; stty sane 2>/dev/null; }
trap cleanup EXIT INT

# ── Fuzzy match (characters match in order, case-insensitive) ──
fuzzy_match() {
    local p="${1,,}" t="${2,,}" pi=0
    for (( i=0; i<${#t}; i++ )); do
        [[ "${t:i:1}" == "${p:pi:1}" ]] && (( pi++ )) || true
        (( pi >= ${#p} )) && return 0
    done
    return 1
}

# ── Scan models dir, apply filter ──
scan() {
    ITEMS=()
    TOTAL=0
    for f in "$MODELS"/*.bin; do
        [[ -f "$f" ]] || continue
        local name="${f##*/}"
        (( TOTAL++ )) || true
        if [[ -n "$FILTER" ]]; then
            fuzzy_match "$FILTER" "$name" || continue
        fi
        ITEMS+=("$name")
    done
    local max=$(( ${#ITEMS[@]} - 1 ))
    (( max < 0 )) && max=0 || true
    (( SEL > max )) && SEL=$max || true
    (( SEL < 0 )) && SEL=0 || true
}

# ── Render ──
render() {
    printf '\e[H\e[2J'

    # Header
    printf '\e[1;36m  MODEL SELECT\e[0m'
    if [[ "$MODE" == "insert" ]]; then
        printf '  \e[33m/%s\e[5m_\e[0m' "$FILTER"
        printf '  \e[2m(%d/%d)\e[0m' "${#ITEMS[@]}" "$TOTAL"
    else
        printf '  \e[2m(%d)\e[0m' "${#ITEMS[@]}"
    fi
    printf '\n  \e[2m────────────────────────────────────────\e[0m\n\n'

    # List
    if (( ${#ITEMS[@]} == 0 )); then
        printf '  \e[2mNo models found\e[0m\n'
    else
        for i in "${!ITEMS[@]}"; do
            if (( i == SEL )); then
                printf '  \e[7;1m > %s \e[0m\n' "${ITEMS[i]}"
            else
                printf '    %s\n' "${ITEMS[i]}"
            fi
        done
    fi

    # Footer
    printf '\n'
    if [[ "$MODE" == "normal" ]]; then
        printf '  \e[2mj/k navigate  i search  ↵ run  q quit\e[0m\n'
    else
        printf '  \e[2mtype to filter  esc back  ↵ run\e[0m\n'
    fi
}

# ── Read single keypress ──
read_key() {
    local k
    IFS= read -rsn1 k 2>/dev/null || { echo "QUIT"; return; }
    if [[ "$k" == $'\e' ]]; then
        local s="" s2=""
        IFS= read -rsn1 -t 0.05 s 2>/dev/null || true
        if [[ -z "$s" ]]; then echo "ESC"; return; fi
        IFS= read -rsn1 -t 0.05 s2 2>/dev/null || true
        case "${s}${s2}" in
            "[A") echo "UP";; "[B") echo "DOWN";;
            *) echo "ESC";;
        esac
        return
    fi
    case "$k" in
        "")              echo "ENTER";;
        $'\x7f'|$'\b')  echo "BACKSPACE";;
        $'\x03')         echo "QUIT";;
        *)               printf '%s' "$k";;
    esac
}

# ── Preflight ──
scan
if (( TOTAL == 0 )); then
    printf 'No .bin models in %s\n' "$MODELS"
    printf 'Download one with: sh src/models/download-ggml-model.sh large-v3\n'
    exit 1
fi

# ── Audio file ──
AUDIO="${1:-}"
if [[ -z "$AUDIO" ]]; then
    printf '\e[1;36m  Usage:\e[0m ./run.sh <audio-file>\n\n'
    printf '  Audio must be 16-bit WAV (16kHz mono).\n'
    printf '  Convert with: ffmpeg -i input.mp3 -ar 16000 -ac 1 -c:a pcm_s16le output.wav\n\n'
    printf '  Continuing to model select — will prompt for file after.\n\n'
fi

# ── Main loop ──
main() {
    printf '\e[?25l'
    render
    while true; do
        local key
        key=$(read_key)

        if [[ "$MODE" == "normal" ]]; then
            case "$key" in
                j|DOWN)  (( SEL < ${#ITEMS[@]} - 1 )) && (( SEL++ )) || true;;
                k|UP)    (( SEL > 0 )) && (( SEL-- )) || true;;
                g)       SEL=0;;
                G)       SEL=$(( ${#ITEMS[@]} - 1 )); (( SEL < 0 )) && SEL=0 || true;;
                i|a|/)   MODE="insert"; FILTER=""; scan;;
                q|QUIT)  exit 0;;
                ENTER)   (( ${#ITEMS[@]} > 0 )) && return 0 || true;;
                ESC)     ;;
            esac
        else
            case "$key" in
                ESC)       MODE="normal";;
                ENTER)     (( ${#ITEMS[@]} > 0 )) && return 0 || true;;
                BACKSPACE) FILTER="${FILTER%?}"; SEL=0; scan;;
                QUIT)      exit 0;;
                *)
                    if [[ ${#key} -eq 1 ]] && [[ "$key" =~ [[:print:]] ]]; then
                        FILTER+="$key"; SEL=0; scan
                    fi
                    ;;
            esac
        fi
        render
    done
}

main

# ── Launch ──
model="${ITEMS[$SEL]}"
cleanup
trap - EXIT INT
printf '\e[H\e[2J'

if [[ -z "$AUDIO" ]]; then
    printf '\e[?25h'
    printf '\e[1;36m  Selected:\e[0m %s\n\n' "$model"
    read -rp '  Audio file path: ' AUDIO
    if [[ -z "$AUDIO" ]]; then
        printf '  No file provided.\n'
        exit 1
    fi
    printf '\n'
fi

printf '\e[1;36m  Transcribing:\e[0m %s\n' "$(basename "$AUDIO")"
printf '\e[1;36m  Model:\e[0m        %s\n\n' "$model"
exec "$BIN/whisper-cli" -m "$MODELS/$model" -f "$AUDIO"
