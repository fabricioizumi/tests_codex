# MPEG1 Player em C++ com libav

Programa simples em C++ para abrir e reproduzir um arquivo de vídeo MPEG1 usando bibliotecas libav (FFmpeg) e SDL2 para exibição.

## Dependências

- libavformat
- libavcodec
- libavutil
- libswscale
- SDL2
- g++ (C++17)
- pkg-config

## Compilação

```bash
make
```

## Uso

```bash
./mpeg1_player caminho/para/video.mpg
```

> O programa exibe um aviso caso o codec do vídeo não seja MPEG1, mas tenta reproduzir mesmo assim.
