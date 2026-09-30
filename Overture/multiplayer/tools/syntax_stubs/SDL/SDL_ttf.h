// syntax-check stub
#pragma once
#include <SDL/SDL.h>
struct TTF_Font;
#define TTF_STYLE_NORMAL 0
#define TTF_STYLE_BOLD 1
#define TTF_STYLE_ITALIC 2
extern "C" {
int TTF_Init(void); void TTF_Quit(void); int TTF_WasInit(void); TTF_Font* TTF_OpenFont(const char*, int); TTF_Font* TTF_OpenFontIndex(const char*, int, long); TTF_Font* TTF_OpenFontRW(SDL_RWops*, int, int); void TTF_CloseFont(TTF_Font*); const char* TTF_GetError(void);
void TTF_SetFontStyle(TTF_Font*, int); int TTF_GetFontStyle(TTF_Font*); int TTF_FontHeight(TTF_Font*); int TTF_FontAscent(TTF_Font*); int TTF_FontDescent(TTF_Font*); int TTF_FontLineSkip(TTF_Font*);
int TTF_GlyphMetrics(TTF_Font*, Uint16, int*, int*, int*, int*, int*); int TTF_SizeText(TTF_Font*, const char*, int*, int*); int TTF_SizeUNICODE(TTF_Font*, const Uint16*, int*, int*);
SDL_Surface* TTF_RenderText_Solid(TTF_Font*, const char*, SDL_Color); SDL_Surface* TTF_RenderText_Blended(TTF_Font*, const char*, SDL_Color); SDL_Surface* TTF_RenderGlyph_Blended(TTF_Font*, Uint16, SDL_Color); SDL_Surface* TTF_RenderGlyph_Solid(TTF_Font*, Uint16, SDL_Color); SDL_Surface* TTF_RenderUNICODE_Blended(TTF_Font*, const Uint16*, SDL_Color); SDL_Surface* TTF_RenderUTF8_Blended(TTF_Font*, const char*, SDL_Color);
}
