# Bash completion for board.
#
# Install it where bash-completion looks for per-command scripts:
#
#   mkdir -p ~/.local/share/bash-completion/completions
#   cp board-completion.bash ~/.local/share/bash-completion/completions/board
#
# or source it from ~/.bashrc:
#
#   source /path/to/board-completion.bash
#
# The file name in the completions directory must be "board" (without .bash)
# for ./board <TAB> to find it through bash-completion's basename fallback.

_board()
{
    local cur prev i word skip=0 positional=0
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    # Arguments of options that take a value.
    case $prev in
        -o|--font)
            COMPREPLY=($(compgen -f -- "$cur"))
            compopt -o filenames 2>/dev/null
            return 0
            ;;
        --format)
            COMPREPLY=($(compgen -W "360p 480p 720p 1080p 1440p 2160p" -- "$cur"))
            return 0
            ;;
        --camera)
            COMPREPLY=($(compgen -W "fit follow" -- "$cur"))
            return 0
            ;;
        --encoder)
            COMPREPLY=($(compgen -W "libx264 h264_nvenc" -- "$cur"))
            return 0
            ;;
        --minutes|--speed|--decimals|--zoom-lead|--auto-speed|--max-auto-speed|--max-scene-seconds|--threads)
            return 0
            ;;
    esac

    local opts="-o --minutes --speed --encoder --decimals --dry --format --expand --show-expanded --syntax-check --stats --only-show-results --font --camera --zoom-lead --auto-speed --max-auto-speed --max-scene-seconds --threads"
    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W "$opts" -- "$cur"))
        return 0
    fi

    # Count the positional arguments before the current word.  The first one is
    # the input .txt, the second one is the output .mp4.
    for ((i = 1; i < COMP_CWORD; i++)); do
        word=${COMP_WORDS[i]}
        if ((skip)); then
            skip=0
            continue
        fi
        case $word in
            -o|--minutes|--speed|--encoder|--decimals|--format|--font|--camera|--zoom-lead|--auto-speed|--max-auto-speed|--max-scene-seconds|--threads)
                skip=1
                ;;
            -*)
                ;;
            *)
                ((positional++))
                ;;
        esac
    done

    local files
    if ((positional == 0)); then
        files=($(compgen -f -X '!*.txt' -- "$cur") $(compgen -d -- "$cur"))
    else
        files=($(compgen -f -X '!*.mp4' -- "$cur") $(compgen -d -- "$cur"))
    fi
    if [[ -z $cur ]]; then
        COMPREPLY=($(compgen -W "$opts" -- "$cur") "${files[@]}")
    else
        COMPREPLY=("${files[@]}")
    fi
    compopt -o filenames 2>/dev/null
    return 0
}

complete -F _board board
complete -F _board ./board
