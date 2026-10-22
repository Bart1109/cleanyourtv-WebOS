#include <SDL.h>
#include <stdio.h>
int main(int argc,char**argv){
  FILE*lg=fopen("/tmp/cytv-native.log","w");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0){fprintf(lg,"init fail %s\n",SDL_GetError());fclose(lg);return 1;}
  fprintf(lg,"driver %s\n",SDL_GetCurrentVideoDriver());
  SDL_Window*w=SDL_CreateWindow("x",0,0,1920,1080,SDL_WINDOW_FULLSCREEN|SDL_WINDOW_SHOWN);
  if(!w){fprintf(lg,"win fail %s\n",SDL_GetError());fclose(lg);return 2;}
  SDL_Renderer*r=SDL_CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!r){fprintf(lg,"ren fail %s\n",SDL_GetError());fclose(lg);return 3;}
  SDL_RendererInfo ri;SDL_GetRendererInfo(r,&ri);int ww,hh;SDL_GetRendererOutputSize(r,&ww,&hh);
  fprintf(lg,"renderer %s %dx%d\n",ri.name,ww,hh);fflush(lg);
  int x=100;Uint32 t0=SDL_GetTicks(),frames=0;int run=1;
  while(run){SDL_Event e;while(SDL_PollEvent(&e)){
    if(e.type==SDL_KEYDOWN){fprintf(lg,"key %d scan %d\n",e.key.keysym.sym,e.key.keysym.scancode);fflush(lg);if(e.key.keysym.sym==SDLK_ESCAPE||e.key.keysym.sym==461||e.key.keysym.sym==SDLK_BACKSPACE)run=0;}
    if(e.type==SDL_QUIT)run=0;}
    x=(x+8)%1700;SDL_SetRenderDrawColor(r,10,10,20,255);SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r,230,60,80,255);SDL_Rect q={x,400,200,200};SDL_RenderFillRect(r,&q);SDL_RenderPresent(r);frames++;
    if(frames==300){fprintf(lg,"300 frames in %u ms\n",SDL_GetTicks()-t0);fflush(lg);}
    if(frames>3000)run=0;}
  fclose(lg);SDL_Quit();return 0;}
