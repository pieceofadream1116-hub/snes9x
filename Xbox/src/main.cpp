// SNES app entry point. SDL3-uwp provides the WinRT entry point, which starts CoreApplication and calls main()
// (renamed to SDL_main by SDL_main.h).

#include "App.h"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	App app;
	return app.Run();
}
