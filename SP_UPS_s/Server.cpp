#include "Server.h"
#include <chrono>
#include <string>


using namespace std;
fd_set Server::client_socks, Server::tests;
struct sockaddr_in Server::peer_addr, Server::client, Server::server;
std::vector<int> players_to_remove;
int MAX_INVALID_MESSAGES = 3;
const int MAX_MSG_SIZE = 1024;
int PING_DIS_TIMER = 230;
int PING_DES_TIMER = 1000;


/**
 * Server initilization
 */
int Server::init(string ip, int port) {
    cout << "IP: " + ip + " Port: " + to_string(port) << endl;

    //Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        cout << "Could not create socket" << endl;
        return -1;
    }

    cout << "Socket created" << endl;

    //set port release
    int enable = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        cout << "setsockopt(REUSEADDR) failed" << endl;
    }



    //Prepare the sockaddr_in structure
    server.sin_family = AF_INET;

    if (ip.compare("localhost") == 0) {
        server.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else if (ip.compare("INADDR_ANY") == 0) {
        server.sin_addr.s_addr = INADDR_ANY;
    } else {
        in_addr_t inAddr = inet_addr(ip.data());
        if (inAddr == -1) {
            cout << "Invalid IP adddress" << endl;
            return -1;
        } else {
            server.sin_addr.s_addr = inAddr;
        }
    }

    server.sin_port = htons(port);

    //Bind
    if (::bind(server_socket, (struct sockaddr *) &server, sizeof(server)) < 0) {
        //print the error message
        cout << "Bind failed. Error" << endl;
        return -1;
    }

    cout << "Bind done" << endl;

    //Listen
    if (listen(server_socket, 3) < 0) {
        cout << "Listen failed. Error" << endl;
        return -1;
    }

    //Accept and incoming connection
    cout << "Server is ready!" << endl;

    return 0;
}


int Server::listenConnections() {

    cout << "Start listening" << endl;

    FD_ZERO(&client_socks);
    FD_SET(server_socket, &client_socks);

    int fd;
    int return_value;

    auto lastPingTime = std::chrono::steady_clock::now();

    while (true) {
        // Check if 5 seconds have passed since the last ping
        auto now = std::chrono::steady_clock::now();
        auto time_diff = std::chrono::duration_cast<std::chrono::seconds>(now - lastPingTime).count();

        if (time_diff >= 5 && !ClientManager::logged_players.empty()) {
            for (auto& pair: ClientManager::logged_players) {
                Player *player = pair.second;
                if (player == nullptr) continue;
                Response::sendToPlayer(player, "PING");
                player->ping_counter++;

                if (player->ping_counter == PING_DIS_TIMER) {
                    disconnect(player->socket);
                }
                else if (player->ping_counter >= PING_DES_TIMER) {
                    players_to_remove.push_back(player->socket);
                }
            }

            for (auto socket : players_to_remove) {
                ClientManager::destroyPlayer(socket);
                cout << "destroy player" << socket << endl;
            }
            players_to_remove.clear();

                lastPingTime = now;
        }

        tests = client_socks;
        struct timeval timeout = {5,0};
        return_value = select(FD_SETSIZE, &tests, (fd_set *) nullptr, (fd_set *) nullptr, &timeout);

        if (return_value < 0) {
            cout << "Select error\n" << endl;
            return -1;
        }
        int a2read;
        char cbuf[1024];
        // vynechavame stdin, stdout, stderr
        for (fd = 3; fd < FD_SETSIZE; fd++) {
            // je dany socket v sade fd ze kterych lze cist ?
            if (FD_ISSET(fd, &tests)) {

                if (fd == server_socket) {
                    socklen_t c = sizeof(struct sockaddr_in);
                    client_socket = accept(server_socket, (struct sockaddr *) &client, &c);
                    if (client_socket < 0) {
                        cout << "Accept error" << endl;
                        return -1;
                    }

                    FD_SET(client_socket, &client_socks);
                    cout << "Connect new client" << endl;

                    char *ip = inet_ntoa(client.sin_addr);

                    ClientManager::create_unlogged_player(ip, client_socket);

                } else
                {
                    ioctl(fd, FIONREAD, &a2read);

                    if (a2read > MAX_MSG_SIZE) {
                        cout << "Přijatá zpráva překračuje maximální povolenou velikost. Odpojování klienta." << endl;
                        Response::sendToClient(fd, "LOGIN|ERR|Přijatá zpráva překračuje maximální povolenou velikost. Odpojování klienta.");
                        closeSocket(fd);
                        continue;
                    }
                    // mame co cist
                    if (a2read > 0) {
                        Player *pl = ClientManager::get_logged_player_by_socket(fd);


                        //clearbuffer
                        memset(cbuf, 0, 1024);

                        recv(fd, &cbuf, 1024, 0);

                        string msg(cbuf);


                        if (pl == nullptr) {
                            pl = ClientManager::get_unlogged_player(fd);
                        }

                        Request::resolve(pl, msg);
                        msg.clear();

                        if (pl->invalid_message_counter >= MAX_INVALID_MESSAGES) {
                            cout << "Maximum invalid messages reached. Player: " + pl->name + " socket: " +
                                    to_string(pl->socket) << endl;
                            pl->invalid_message_counter--;
                            Response::sendErrorToPlayer(pl,"END|max invalid messages reached");
                            pl->invalid_message_counter = 0;
                            auto game = GameManager::gameExists(pl->game_id);
                            if (game != nullptr) {
                                if(game->gameState == "WAITING") GameManager::playerDequeue(pl);
                                if(game->gameState == "RUNNING") GameManager::playerLeaveGame(pl);
                            }
                            disconnect(fd);
                            ClientManager::destroyPlayer(fd);
                        }


                    } else {
                        close(fd);
                        FD_CLR(fd, &client_socks);

                        cout << "Something bad happened on client" << endl;
                    }

                }

            }
        }


    }
}

void Server::disconnect(int socket) {
    Player *pl = ClientManager::get_logged_player_by_socket(socket);

    if (pl == nullptr) {
        closeSocket(socket);
        return;
    }

    pl->connected = -1;
    closeSocket(socket);
    auto game = GameManager::gameExists(pl->game_id);
    if (game != nullptr) {
        GameManager::notifyOponent(pl, "DISCON");
    }
    if(pl->invalid_message_counter >= MAX_INVALID_MESSAGES){
        ClientManager::logged_players.erase(pl->name);
    }

}


void Server::closeSocket(int socket) {
    cout << "Closing socket with ID: " + to_string(socket) << endl;

    close(socket);
    FD_CLR(socket, &client_socks);

}


