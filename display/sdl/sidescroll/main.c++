#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <algorithm>
#include <boost/algorithm/string.hpp>

#define POSERSPACE_PORT 9050
#define EVENT_COUNT 8
#define FRAMEDELAY 25000
#define WIDTH 1920
#define HEIGHT 1080

struct Line {
  TTF_Font *font;
  float x, y, w, h;
  std::string content;
  SDL_Surface *surface;
  
  struct {
    int r, g, b;
  } col;
};

std::vector<Line> lines;
std::vector<TTF_Font *> fonts;

int prepareSocket() {
  const int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if(serverSocket < 0) throw "socket(2) failed";

  int enable = 1;
  if(setsockopt(serverSocket, SOL_IP, SO_REUSEADDR, &enable, sizeof(enable)) < 0) throw "setsockopt(2) failed";

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(POSERSPACE_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if(bind(serverSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(sockaddr_in)) < 0) throw "bind(2) failed";
  if(listen(serverSocket, 2) < 0) throw "listen(2) failed";
  return serverSocket;
}

int prepareEpoll(const int listeningSocket) {
  const int poll = epoll_create(8);
  if(poll < 0) throw "epoll_create(2) failed";

  epoll_event add;
  add.events = EPOLLIN;
  add.data.fd = listeningSocket;

  if(epoll_ctl(poll, EPOLL_CTL_ADD, listeningSocket, &add) < 0) throw "epoll_ctl(2) failed";
  return poll;
}

class DataInterpreter {
  public:
    DataInterpreter() { }
    virtual ~DataInterpreter() { }
    virtual void handleData(const std::vector<std::string> &) = 0;
};

int maxsize(int lineCount) {
  if(lineCount < 20) return 28;
  if(lineCount < 25) return (rand() % 10)? 24: 28;
  if(lineCount < 30) return (rand() % 15)? 20: 28;
  if(lineCount < 50) return (rand() % 20)? 16: 24;
  if(lineCount < 80) return (rand() % 40)? 12: 24;
  if(lineCount < 110) return (rand() % 50)? 8: 24;
  if(lineCount < 200) return (rand() % 100)? 4: 24;
  return (rand() % lineCount)? 1: 24;
}

class TextInterpreter: public DataInterpreter {
  public:
    virtual void handleData(const std::vector<std::string> &line) {
      if(line[0] == "") return;

      Line l;
      l.surface = nullptr;
      // l.h = rand() % 28 + 4;
      l.h = rand() % maxsize(lines.size()) + 4;
      l.font = fonts[l.h];
      l.x = WIDTH;
      l.y = rand() % HEIGHT;
      l.w = WIDTH * 2;
      l.col.r = 0;
      // l.col.g = 64 + rand() % 191;
      l.col.g = 64 + 191 * l.h / 32 - (rand() % 32);
      l.col.b = 0;
      l.content = line[0];
      lines.push_back(l);
    }
};

class ConnectionState {
  private: 
    int fd;
    std::string buf;

    enum {
      ACTION,
      HEADER,
      DATA
    } state;

    std::shared_ptr<DataInterpreter> interpreter;

  public:
    explicit ConnectionState(const int fd): fd(fd), state(ACTION), interpreter(0) { }

    void handleHeader(const std::string &header, const std::string &value) {
      std::cerr << "Header: " << header << " => " << value << std::endl;
      if(header == "Content-type") {
        if(value == "x-poserspace/text") interpreter = std::make_shared<TextInterpreter>();
      }
    }

    void handleLine(const int, const std::string &line) {
      switch(state) {
        case ACTION:
          state = HEADER;
          break;
        case HEADER:
          if(line == "") {
            state = DATA;
          } else {
            auto colon = std::find(line.begin(), line.end(), ':');
            if(colon == line.end()) throw "invalid header: " + line;
            auto header = std::string(line.begin(), colon);
            auto headerValue = boost::algorithm::trim_left_copy(std::string(++colon, line.end()));
            handleHeader(header, headerValue);
          }
          break;
        case DATA:
          std::cerr << line << std::endl;

          std::vector<std::string> values;
          boost::algorithm::split(values, line, [](char c) { return c == '\t'; });
          interpreter->handleData(values);
          break;
      }
    }

    void handleInput(const std::string &input) {
      buf += input;

      while(1) {
        auto nl = std::find(buf.begin(), buf.end(), '\n');
        if(nl == buf.end()) break;
        auto e = nl;
        if(*(e - 1) == '\r') --e;

        handleLine(fd, std::string(buf.begin(), e));
        buf = std::string(++nl, buf.end());
      }
    }
};

std::vector<ConnectionState> connections;

int acceptConnection(const int listeningSocket, const int poll) {
  const int ret = accept(listeningSocket, nullptr, nullptr);
  if(ret < 0) throw "accept(2) failed";
  const unsigned int con = ret;

  epoll_event add;
  add.events = EPOLLIN;
  add.data.fd = con;
  if(epoll_ctl(poll, EPOLL_CTL_ADD, con, &add) < 0) throw "epoll_ctl(2) failed";

  while(connections.size() < con) connections.push_back(ConnectionState(-1));
  connections.push_back(ConnectionState(con));

  return con;
}

void acceptInput(const int fd, const int poll) {
  char buffer[4096];
  int len = read(fd, buffer, sizeof(buffer));
  if(len < 0) {
    if(epoll_ctl(poll, EPOLL_CTL_DEL, 0, 0) < 0) throw "epoll_ctl(2) failed";
    close(fd);
    connections[fd] = ConnectionState(-1);
  } else {
    connections[fd].handleInput(std::string(buffer, len));
  }
}

uint64_t now() {
  timeval tv;
  if(gettimeofday(&tv, nullptr) < 0) throw "gettimeofday(2) failed";

  return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

void renderFrame(SDL_Surface *screen, SDL_Renderer *renderer) {
  {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    std::vector<Line> living;

    for(auto &l: lines) {
      if(!l.surface) {
        l.surface = TTF_RenderUTF8_Blended(l.font, l.content.c_str(), SDL_Color{
            static_cast<uint8_t>(l.col.r),
            static_cast<uint8_t>(l.col.g),
            static_cast<uint8_t>(l.col.b),
            128});
        if(!l.surface) throw "TTF_RenderUTF8_Solid failed";
      }

      if(l.surface->w > 0 && l.surface->h > 0) {
        SDL_Rect r{static_cast<int>(l.x), static_cast<int>(l.y), l.surface->w, l.surface->h};
        SDL_BlitSurface(l.surface, nullptr, screen, &r);
      }

      l.w = l.surface->w;

      // l.x -= 0.1 + l.h / 4.0;
      // l.x -= 0.1 + l.w / 64.0;
      float dx = 0.1 + l.w / 64.0;
      while(dx > 20) dx /= 10;
      l.x -= dx;

      if(l.x > -l.w) {
        living.push_back(l);
      } else {
        SDL_FreeSurface(l.surface);
      }
    }

    lines = living;
  }

  SDL_RenderPresent(renderer);
}

int main(int, char **) {
  try {
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE)) throw "SDL init failed";
    if(TTF_Init() < 0) throw "TTF init failed";

    SDL_Window *const window = SDL_CreateWindow("-[ data ]-",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIDTH, HEIGHT, 0);
    SDL_Surface *const screen = SDL_GetWindowSurface(window);
    SDL_Renderer *const renderer = SDL_CreateSoftwareRenderer(screen);

    fonts.push_back(nullptr);
    for(int i = 1; i < 32; ++i) {
      fonts.push_back(TTF_OpenFont("8bitoperator.ttf", i));
      if(!fonts.back()) throw "TTF_OpenFont failed";
    }

    const int serverSocket = prepareSocket();
    const int poll = prepareEpoll(serverSocket);
    bool running = true;
    uint64_t nextFrameAt = now() + FRAMEDELAY;

    while(running) {
      const auto t = now();

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if(event.type == SDL_QUIT) {
          running = false;
        }
      }

      if(nextFrameAt < t) {
        renderFrame(screen, renderer);
        SDL_UpdateWindowSurface(window);

        nextFrameAt = t + FRAMEDELAY;
      }

      epoll_event events[EVENT_COUNT];
      // std::cerr << nextFrameAt - t << std::endl;
      const int num = epoll_wait(poll, events, EVENT_COUNT, (nextFrameAt - t) / 1000);
      if(num < 0) throw "epoll_wait(2) failed";

      for(int i = 0; i < num; ++i) {
        if(events[i].data.fd == serverSocket) {
          acceptConnection(serverSocket, poll);
        } else {
          acceptInput(events[i].data.fd, poll);
        }
      }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
  } catch (const std::string &err) {
    std::cerr << err << std::endl;
    std::cerr << SDL_GetError() << std::endl;
    std::cerr << TTF_GetError() << std::endl;
    std::cerr << strerror(errno) << std::endl;
  } catch (const char *&err) {
    std::cerr << err << std::endl;
    std::cerr << SDL_GetError() << std::endl;
    std::cerr << TTF_GetError() << std::endl;
    std::cerr << strerror(errno) << std::endl;
    return 1;
  }
}
