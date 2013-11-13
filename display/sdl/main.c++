#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
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
#define WIDTH 1024
#define HEIGHT 768

struct GeoData {
  SDL_Surface *earth;

  double targetLat, targetLon;
  double currentLat, currentLon;
};

GeoData geo;

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

class GeoInterpreter: public DataInterpreter {
  public:
    virtual void handleData(const std::vector<std::string> &data) {
      std::cerr << "Geo: " << data[0] << "," << data[1] << std::endl;
    }
};

class TextInterpreter: public DataInterpreter {
  public:
    virtual void handleData(const std::vector<std::string> &) { }
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
        if(value == "x-poserspace/geo") interpreter = std::make_shared<GeoInterpreter>();
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

          geo.targetLat = atof(values[0].c_str());
          geo.targetLon = atof(values[1].c_str());
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

double lon2X(double lon) {
  return (lon + 180) / 360 * WIDTH;
}

double lat2Y(double lat) {
  return HEIGHT / 2 + HEIGHT / 1.7 / M_PI * (log(tan(M_PI / 4 + lat / 180 * M_PI / 2)));
}

void renderFrame(SDL_Surface *screen, SDL_Renderer *renderer) {
  SDL_BlitScaled(geo.earth, nullptr, screen, nullptr);

  const double x = lon2X(geo.currentLon);
  const double y = lat2Y(geo.currentLat);

  geo.currentLat = (geo.currentLat * 0.9 + geo.targetLat * 0.1);
  geo.currentLon = (geo.currentLon * 0.9 + geo.targetLon * 0.1);

  const double dLat = geo.currentLat - geo.targetLat;
  const double dLon = geo.currentLon - geo.targetLon;

  if(dLat * dLat + dLon * dLon < 1 && (now() / 100000) % 2) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
  } else {
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
  }

  SDL_RenderDrawLine(renderer, 0, y, WIDTH, y);
  SDL_RenderDrawLine(renderer, x, 0, x, HEIGHT);

  SDL_RenderPresent(renderer);
}

SDL_Surface *loadEarth() {
  SDL_Surface *const img = IMG_Load("earth.png");
  SDL_Surface *const earth = SDL_CreateRGBSurface(0, WIDTH, HEIGHT, 32, 0xff, 0xff00, 0xff0000, 0xff000000);

  SDL_BlitScaled(img, nullptr, earth, nullptr);
  SDL_FreeSurface(img);
  return earth;
}

int main(int, char **) {
  try {
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE)) throw "SDL init failed";
    SDL_Window *const window = SDL_CreateWindow("-[ data ]-",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIDTH, HEIGHT, 0);
    SDL_Surface *const screen = SDL_GetWindowSurface(window);
    SDL_Renderer *const renderer = SDL_CreateSoftwareRenderer(screen);

    // geo.earth = IMG_Load("earth.jpg");
    geo.earth = loadEarth();

    geo.currentLat = geo.currentLon = 0.0;
    geo.targetLat = geo.targetLon = 0.0;

    geo.targetLat = -52.26471465026548;
    geo.targetLon = 10.515537294323199;

    const int serverSocket = prepareSocket();
    const int poll = prepareEpoll(serverSocket);
    bool running = true;
    uint64_t nextFrameAt = now() + FRAMEDELAY;

    while(running) {
      const auto t = now();

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
    std::cerr << strerror(errno) << std::endl;
  } catch (const char *&err) {
    std::cerr << err << std::endl;
    std::cerr << SDL_GetError() << std::endl;
    std::cerr << strerror(errno) << std::endl;
    return 1;
  }
}
