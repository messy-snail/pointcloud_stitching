#include <librealsense2/rs.hpp>

#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <cstring>
#include <stdlib.h>
#include <vector>
#include <iostream>
#include <stdio.h>
#include <chrono>

const int PORT = 8000;
const int BUF_SIZE = 4000000;
const int CONV_RATE = 1000;

int client_sock = 0;
int sockfd = 0;
short * buffer;

void sigintHandler(int dummy) {
    close(client_sock);
    close(sockfd);
    free(buffer);
}

void initSocket(int port) {
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        std::cerr << "\nSocket fd not received." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "\nBind failed" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (listen(sockfd, 3) < 0) {
        std::cerr << "\nListen failed" << std::endl;
        exit(EXIT_FAILURE);
    }

    std::cout << "Waiting for client..." << std::endl;

    if ((client_sock = accept(sockfd, NULL, NULL)) < 0) {
        std::cerr << "\nConnection failed" << std::endl;
        exit(EXIT_FAILURE);
    }

    std::cout << "Established connection with client_sock: " << client_sock << std::endl;
}

std::tuple<uint8_t, uint8_t, uint8_t> get_texcolor(rs2::video_frame texture, rs2::texture_coordinate texcoords) {

    // auto write_to_buffer_start = std::chrono::high_resolution_clock::now();
    const int w = texture.get_width(), h = texture.get_height();
    int x = std::min(std::max(int(texcoords.u*w + .5f), 0), w - 1);
    int y = std::min(std::max(int(texcoords.v*h + .5f), 0), h - 1);
    int idx = x * texture.get_bytes_per_pixel() + y * texture.get_stride_in_bytes();
    const auto texture_data = reinterpret_cast<const uint8_t*>(texture.get_data());
    // auto write_to_buffer_end = std::chrono::high_resolution_clock::now();
    // std::cout << "#: " << std::chrono::duration<double, std::milli>(write_to_buffer_end - write_to_buffer_start).count() << std::endl;

    return std::tuple<uint8_t, uint8_t, uint8_t>(texture_data[idx], texture_data[idx + 1], texture_data[idx + 2]);
}

int copyPointCloudXYZRGBToBuffer(rs2::points& pts, const rs2::video_frame& color, short * pc_buffer) {
    auto vertices = pts.get_vertices();
    auto tex_coords = pts.get_texture_coordinates();
    int size = 0;

    for (size_t i = 0; i < pts.size() && (5 * size + 2) < BUF_SIZE; i++) {
        if ((vertices[i].x != 0) && (vertices[i].x < 2) && (vertices[i].x > -2) && (vertices[i].z != 0) && (vertices[i].z < 3)) {
            std::tuple<uint8_t, uint8_t, uint8_t> current_color = get_texcolor(color, tex_coords[i]);
            pc_buffer[size * 5 + 0] = static_cast<short>(vertices[i].x * CONV_RATE);
            pc_buffer[size * 5 + 1] = static_cast<short>(vertices[i].y * CONV_RATE);
            pc_buffer[size * 5 + 2] = static_cast<short>(vertices[i].z * CONV_RATE);
            pc_buffer[size * 5 + 3] = (short)std::get<0>(current_color) + (short)(std::get<1>(current_color) << 8);
            pc_buffer[size * 5 + 4] = std::get<2>(current_color);

            size++;
        }
    }

    return size;
}

int copyPointCloudXYZToBuffer(rs2::points& pts, short * pc_buffer) {
    auto vertices = pts.get_vertices();
    int size = 0;

    for (size_t i = 0; i < pts.size() && (3 * size + 2) < BUF_SIZE; i++) {
        if ((vertices[i].x != 0) && (vertices[i].x < 2) && (vertices[i].x > -2) && (vertices[i].z != 0) && (vertices[i].z < 2)) {
            pc_buffer[size * 3 + 0] = static_cast<short>(vertices[i].x * CONV_RATE);
            pc_buffer[size * 3 + 1] = static_cast<short>(vertices[i].y * CONV_RATE);
            pc_buffer[size * 3 + 2] = static_cast<short>(vertices[i].z * CONV_RATE);

            size++;
        }
    }

    return size;
}

int main (int argc, char** argv) {
    char pull_request[1] = {0};
    buffer = (short *)malloc(sizeof(short) * BUF_SIZE);

    rs2::pointcloud pc;
    rs2::pipeline pipe;
    rs2::pipeline_profile selection = pipe.start();
    rs2::device selected_device = selection.get_device();
    auto depth_sensor = selected_device.first<rs2::depth_sensor>();

    if (depth_sensor.supports(RS2_OPTION_EMITTER_ENABLED))
        depth_sensor.set_option(RS2_OPTION_EMITTER_ENABLED, 0.f);

    initSocket(atoi(argv[1]));      
    signal(SIGINT, sigintHandler);

    while (1) {

        // auto loop_start = std::chrono::high_resolution_clock::now();
        if (recv(client_sock, pull_request, 1, 0) < 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }
        if (pull_request[0] == 'Y') {
            auto frames = pipe.wait_for_frames();
            auto depth = frames.get_depth_frame();
            auto pts = pc.calculate(depth);

            int size = copyPointCloudXYZToBuffer(pts, &buffer[0] + sizeof(short));
            size = 3 * size * sizeof(short);
            memcpy(buffer, &size, sizeof(int));

            send(client_sock, (char *)buffer, size + sizeof(int), 0);
        }
        else if (pull_request[0] == 'Z') {          // Get XYZRGB point clouds
            // auto grab_frame_start = std::chrono::high_resolution_clock::now();
            auto frames = pipe.wait_for_frames();
            auto depth = frames.get_depth_frame();
            auto color = frames.get_color_frame();
            auto pts = pc.calculate(depth);
            pc.map_to(color);
            // auto grab_frame_end = std::chrono::high_resolution_clock::now();
            // std::cout << "Frame grabbing: " << std::chrono::duration<double, std::milli>(grab_frame_end - grab_frame_start).count() << std::endl;

            // auto write_to_buffer_start = std::chrono::high_resolution_clock::now();
            int size = copyPointCloudXYZRGBToBuffer(pts, color, &buffer[0] + sizeof(short));        // Size = number of shorts in buffer
            size = 5 * size * sizeof(short);
            memcpy(buffer, &size, sizeof(int));
            // auto write_to_buffer_end = std::chrono::high_resolution_clock::now();
            // std::cout << "Writing to buffer: " << std::chrono::duration<double, std::milli>(write_to_buffer_end - write_to_buffer_start).count() << std::endl;

            // auto send_start = std::chrono::high_resolution_clock::now();
            send(client_sock, (char *)buffer, size + sizeof(int), 0);
            // auto send_end = std::chrono::high_resolution_clock::now();
            // std::cout << "Sending data: " << std::chrono::duration<double, std::milli>(send_end - send_start).count() << std::endl;
        }
        else {
            std::cerr << "Faulty pull request" << std::endl;
            exit(EXIT_FAILURE);
        }
        // auto loop_end = std::chrono::high_resolution_clock::now();
        // std::cout << "TOTAL LOOP TIME: " << std::chrono::duration<double, std::milli>(loop_end - loop_start).count() << "\n" << std::endl;

    }

    close(client_sock);
    close(sockfd);
    free(buffer);
    return 0;
}
