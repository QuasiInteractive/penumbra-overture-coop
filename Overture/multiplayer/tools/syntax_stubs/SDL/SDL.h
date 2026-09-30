// syntax-check stub
#pragma once
#include <cstdint>
#include <cstddef>
typedef uint8_t Uint8; typedef int8_t Sint8; typedef uint16_t Uint16; typedef int16_t Sint16; typedef uint32_t Uint32; typedef int32_t Sint32; typedef uint64_t Uint64; typedef int64_t Sint64;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;
typedef int SDLKey; typedef int SDLMod;
struct SDL_PixelFormat { Uint8 BitsPerPixel, BytesPerPixel; Uint32 Rmask, Gmask, Bmask, Amask; Uint8 Rshift, Gshift, Bshift, Ashift, Rloss, Gloss, Bloss, Aloss; Uint32 colorkey; Uint8 alpha; void* palette; };
struct SDL_Rect { Sint16 x, y; Uint16 w, h; };
struct SDL_Color { Uint8 r, g, b, unused; };
struct SDL_Surface { Uint32 flags; SDL_PixelFormat* format; int w, h; Uint16 pitch; void* pixels; SDL_Rect clip_rect; int refcount; };
struct SDL_Joystick; struct SDL_mutex; struct SDL_Thread; struct SDL_cond; struct SDL_sem; struct SDL_RWops;
struct SDL_keysym { Uint8 scancode; SDLKey sym; SDLMod mod; Uint16 unicode; };
struct SDL_KeyboardEvent { Uint8 type, which, state; SDL_keysym keysym; };
struct SDL_MouseMotionEvent { Uint8 type, which, state; Uint16 x, y; Sint16 xrel, yrel; };
struct SDL_MouseButtonEvent { Uint8 type, which, button, state; Uint16 x, y; };
struct SDL_JoyAxisEvent { Uint8 type, which, axis; Sint16 value; };
struct SDL_JoyButtonEvent { Uint8 type, which, button, state; };
struct SDL_ResizeEvent { Uint8 type; int w, h; };
struct SDL_ActiveEvent { Uint8 type, gain, state; };
struct SDL_QuitEvent { Uint8 type; };
struct SDL_UserEvent { Uint8 type; int code; void* data1; void* data2; };
union SDL_Event { Uint8 type; SDL_ActiveEvent active; SDL_KeyboardEvent key; SDL_MouseMotionEvent motion; SDL_MouseButtonEvent button; SDL_JoyAxisEvent jaxis; SDL_JoyButtonEvent jbutton; SDL_ResizeEvent resize; SDL_QuitEvent quit; SDL_UserEvent user; };
enum { SDL_NOEVENT = 0, SDL_ACTIVEEVENT, SDL_KEYDOWN, SDL_KEYUP, SDL_MOUSEMOTION, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_JOYAXISMOTION, SDL_JOYBALLMOTION, SDL_JOYHATMOTION, SDL_JOYBUTTONDOWN, SDL_JOYBUTTONUP, SDL_QUIT, SDL_SYSWMEVENT, SDL_VIDEORESIZE = 16, SDL_VIDEOEXPOSE, SDL_USEREVENT = 24 };
enum { SDLK_UNKNOWN = 0, SDLK_LAST = 323 };
enum { KMOD_NONE = 0, KMOD_LSHIFT = 1, KMOD_RSHIFT = 2, KMOD_LCTRL = 64, KMOD_RCTRL = 128, KMOD_LALT = 256, KMOD_RALT = 512, KMOD_SHIFT = 3, KMOD_CTRL = 192, KMOD_ALT = 768 };
#define SDL_INIT_TIMER 1
#define SDL_INIT_AUDIO 16
#define SDL_INIT_VIDEO 32
#define SDL_INIT_JOYSTICK 512
#define SDL_INIT_EVERYTHING 0xFFFF
#define SDL_SWSURFACE 0
#define SDL_HWSURFACE 1
#define SDL_OPENGL 2
#define SDL_FULLSCREEN 0x80000000
#define SDL_SRCALPHA 0x10000
#define SDL_SRCCOLORKEY 0x1000
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_WHEELUP 4
#define SDL_BUTTON_WHEELDOWN 5
#define SDL_BUTTON(x) (1 << ((x)-1))
#define SDL_ENABLE 1
#define SDL_DISABLE 0
#define SDL_QUERY -1
#define SDL_GRAB_ON 1
#define SDL_GRAB_OFF 0
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_SWAP_CONTROL 16
#define SDL_GL_MULTISAMPLEBUFFERS 13
#define SDL_GL_MULTISAMPLESAMPLES 14
#define SDL_GL_RED_SIZE 0
#define SDL_GL_GREEN_SIZE 1
#define SDL_GL_BLUE_SIZE 2
#define SDL_GL_ALPHA_SIZE 3
#define SDL_GL_DEPTH_SIZE 6
#define SDL_GL_STENCIL_SIZE 7
#define SDL_GL_BUFFER_SIZE 4
#define SDL_APPACTIVE 4
#define SDL_APPINPUTFOCUS 2
#define SDL_APPMOUSEFOCUS 1
typedef int SDL_GLattr;
typedef int SDL_GrabMode;
#define SDL_RWFromFile(a,b) ((SDL_RWops*)0)
extern "C" {
int SDL_Init(Uint32); void SDL_Quit(void); int SDL_InitSubSystem(Uint32); void SDL_QuitSubSystem(Uint32); const char* SDL_GetError(void); void SDL_SetError(const char*, ...);
Uint32 SDL_GetTicks(void); void SDL_Delay(Uint32); int SDL_PollEvent(SDL_Event*); int SDL_PumpEvents(void); int SDL_WaitEvent(SDL_Event*); int SDL_PushEvent(SDL_Event*); Uint8 SDL_GetAppState(void);
SDL_Surface* SDL_SetVideoMode(int,int,int,Uint32); SDL_Surface* SDL_GetVideoSurface(void); int SDL_GL_SetAttribute(SDL_GLattr,int); int SDL_GL_GetAttribute(SDL_GLattr,int*); void SDL_GL_SwapBuffers(void); void SDL_WM_SetCaption(const char*,const char*); int SDL_WM_ToggleFullScreen(SDL_Surface*); int SDL_WM_IconifyWindow(void); SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode);
int SDL_ShowCursor(int); Uint8 SDL_GetMouseState(int*,int*); Uint8 SDL_GetRelativeMouseState(int*,int*); void SDL_WarpMouse(Uint16,Uint16); Uint8* SDL_GetKeyState(int*); SDLMod SDL_GetModState(void); int SDL_EnableUNICODE(int); int SDL_EnableKeyRepeat(int,int); char* SDL_GetKeyName(SDLKey);
SDL_Surface* SDL_CreateRGBSurface(Uint32,int,int,int,Uint32,Uint32,Uint32,Uint32); SDL_Surface* SDL_CreateRGBSurfaceFrom(void*,int,int,int,int,Uint32,Uint32,Uint32,Uint32); void SDL_FreeSurface(SDL_Surface*); int SDL_LockSurface(SDL_Surface*); void SDL_UnlockSurface(SDL_Surface*); int SDL_BlitSurface(SDL_Surface*,SDL_Rect*,SDL_Surface*,SDL_Rect*); int SDL_FillRect(SDL_Surface*,SDL_Rect*,Uint32); int SDL_SetAlpha(SDL_Surface*,Uint32,Uint8); int SDL_SetColorKey(SDL_Surface*,Uint32,Uint32); SDL_Surface* SDL_ConvertSurface(SDL_Surface*,SDL_PixelFormat*,Uint32); SDL_Surface* SDL_DisplayFormatAlpha(SDL_Surface*); Uint32 SDL_MapRGBA(const SDL_PixelFormat*,Uint8,Uint8,Uint8,Uint8);
int SDL_NumJoysticks(void); SDL_Joystick* SDL_JoystickOpen(int); void SDL_JoystickClose(SDL_Joystick*); const char* SDL_JoystickName(int); int SDL_JoystickNumAxes(SDL_Joystick*); int SDL_JoystickNumButtons(SDL_Joystick*); int SDL_JoystickNumHats(SDL_Joystick*); int SDL_JoystickNumBalls(SDL_Joystick*); Sint16 SDL_JoystickGetAxis(SDL_Joystick*,int); Uint8 SDL_JoystickGetButton(SDL_Joystick*,int); Uint8 SDL_JoystickGetHat(SDL_Joystick*,int); void SDL_JoystickUpdate(void); int SDL_JoystickEventState(int);
SDL_mutex* SDL_CreateMutex(void); int SDL_mutexP(SDL_mutex*); int SDL_mutexV(SDL_mutex*); void SDL_DestroyMutex(SDL_mutex*); SDL_Thread* SDL_CreateThread(int (*)(void*), void*); void SDL_WaitThread(SDL_Thread*, int*); void SDL_KillThread(SDL_Thread*); Uint32 SDL_ThreadID(void);
int SDL_GetWMInfo(void*); int SDL_putenv(const char*); char* SDL_getenv(const char*);
}
#define SDL_LockMutex(m) SDL_mutexP(m)
#define SDL_UnlockMutex(m) SDL_mutexV(m)
