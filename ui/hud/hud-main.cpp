/*
 * Displays plane HUD along with results from octomap.
 *
 * Author: Andrew Barry, <abarry@csail.mit.edu> 2014
 *
 */

#include "hud-main.hpp"

using namespace std;

lcm_t * lcm;

// globals for subscription functions, so we can unsubscribe in the control-c handler
mav_pose_t_subscription_t *mav_pose_t_sub;
mav_pose_t_subscription_t *mav_pose_t_replay_sub;
lcmt_baro_airspeed_subscription_t *baro_airspeed_sub;
lcmt_battery_status_subscription_t *battery_status_sub;
lcmt_deltawing_u_subscription_t *servo_out_sub;
mav_gps_data_t_subscription_t *mav_gps_data_t_sub;
bot_core_image_t_subscription_t *stereo_image_left_sub;
lcmt_stereo_subscription_t *stereo_replay_sub;
lcmt_stereo_subscription_t *stereo_sub;
lcmt_stereo_with_xy_subscription_t *stereo_xy_sub;
lcmt_stereo_subscription_t *stereo_bm_sub;
octomap_raw_t_subscription_t *octomap_sub;

mutex image_mutex;
Mat left_image = Mat::zeros(240, 376, CV_8UC1); // global so we can update it in the stereo handler and in the main loop

ofstream box_file;

mutex stereo_mutex, stereo_bm_mutex, stereo_xy_mutex, ui_box_mutex;
lcmt_stereo *last_stereo_msg, *last_stereo_bm_msg;
lcmt_stereo_with_xy *last_stereo_xy_msg;

OcTree *octree = NULL;
mutex octomap_mutex;

bool ui_box = false;
bool ui_box_first_click = false;
bool ui_box_done = false;


Point2d box_top(-1, -1);
Point2d box_bottom(-1, -1);

int main(int argc,char** argv) {

    string config_file = "";
    int move_window_x = -1, move_window_y = -1;
    bool replay_hud_bool = false;
    bool record_hud = false;
    bool show_unremapped = false;
    string ui_box_path = ""; // a mode that lets the user draw boxes on screen to select relevant parts of the image


    ConciseArgs parser(argc, argv);
    parser.add(config_file, "c", "config", "Configuration file containing camera GUIDs, etc.", true);
    parser.add(move_window_x, "x", "move-window-x", "Move window starting location x (must pass both x and y)");
    parser.add(move_window_y, "y", "move-window-y", "Move window starting location y (must pass both x and y)");
    parser.add(replay_hud_bool, "r", "replay-hud", "Enable a second HUD on the channel STATE_ESTIMATOR_POSE_REPLAY");
    parser.add(record_hud, "R", "record-hud", "Enable recording to disk.");
    parser.add(show_unremapped, "u", "show-unremapped", "Show the unremapped image");
    parser.add(ui_box_path, "b", "draw-box", "Path to write box drawing results to.");
    parser.parse();

    OpenCvStereoConfig stereo_config;

    // parse the config file
    if (ParseConfigFile(config_file, &stereo_config) != true)
    {
        fprintf(stderr, "Failed to parse configuration file, quitting.\n");
        return 1;
    }

    // load calibration
    OpenCvStereoCalibration stereo_calibration;


    if (LoadCalibration(stereo_config.calibrationDir, &stereo_calibration) != true)
    {
        cerr << "Error: failed to read calibration files. Quitting." << endl;
        return 1;
    }

    if (ui_box_path != "") {
        // init box parsing

        // open a file to write to
        ui_box = true;
        show_unremapped = true;
        box_file.open(ui_box_path);
    }

    lcm = lcm_create ("udpm://239.255.76.67:7667?ttl=0");
    if (!lcm)
    {
        fprintf(stderr, "lcm_create for recieve failed.  Quitting.\n");
        return 1;
    }

    BotParam *param = bot_param_new_from_server(lcm, 0);
    if (param == NULL) {
        fprintf(stderr, "Error: no param server!\n");
        return 1;
    }

    BotFrames *bot_frames = bot_frames_new(lcm, param);

    RecordingManager recording_manager;
    recording_manager.Init(stereo_config);

    // create a HUD object so we can pass it's pointer to the lcm handlers
    Hud hud;
    Hud replay_hud(Scalar(0, 0, 0.8));
    replay_hud.SetClutterLevel(99);

    // if a channel exists, subscribe to it
    char *pose_channel;
    if (bot_param_get_str(param, "coordinate_frames.body.pose_update_channel", &pose_channel) >= 0) {
        mav_pose_t_sub = mav_pose_t_subscribe(lcm, pose_channel, &mav_pose_t_handler, &hud);
    }

    if (replay_hud_bool) {
        mav_pose_t_replay_sub = mav_pose_t_subscribe(lcm, "STATE_ESTIMATOR_POSE_REPLAY", &mav_pose_t_handler, &replay_hud); // we can use the same handler, and just send it the replay_hud pointer
    }

    char *gps_channel;
    if (bot_param_get_str(param, "lcm_channels.gps", &gps_channel) >= 0) {
        mav_gps_data_t_sub = mav_gps_data_t_subscribe(lcm, gps_channel, &mav_gps_data_t_handler, &hud);
    }

    char *baro_airspeed_channel;
    if (bot_param_get_str(param, "lcm_channels.baro_airspeed", &baro_airspeed_channel) >= 0) {
        baro_airspeed_sub = lcmt_baro_airspeed_subscribe(lcm, baro_airspeed_channel, &baro_airspeed_handler, &hud);
    }

    char *servo_out_channel;
    if (bot_param_get_str(param, "lcm_channels.servo_out", &servo_out_channel) >= 0) {
        servo_out_sub = lcmt_deltawing_u_subscribe(lcm, servo_out_channel, &servo_out_handler, &hud);
    }

    char *battery_status_channel;
    if (bot_param_get_str(param, "lcm_channels.battery_status", &battery_status_channel) >= 0) {
        battery_status_sub = lcmt_battery_status_subscribe(lcm, battery_status_channel, &battery_status_handler, &hud);
    }

    char *stereo_replay_channel;
    if (bot_param_get_str(param, "lcm_channels.stereo_replay", &stereo_replay_channel) >= 0) {
        stereo_replay_sub = lcmt_stereo_subscribe(lcm, stereo_replay_channel, &stereo_replay_handler, &hud);
    }

    char *stereo_channel;
    if (bot_param_get_str(param, "lcm_channels.stereo", &stereo_channel) >= 0) {
        stereo_sub = lcmt_stereo_subscribe(lcm, stereo_channel, &stereo_handler, NULL);
    }

    char *stereo_xy_channel;
    if (bot_param_get_str(param, "lcm_channels.stereo_with_xy", &stereo_xy_channel) >= 0) {
        stereo_xy_sub = lcmt_stereo_with_xy_subscribe(lcm, stereo_xy_channel, &stereo_xy_handler, NULL);
    }

    char *stereo_image_left_channel;
    if (bot_param_get_str(param, "lcm_channels.stereo_image_left", &stereo_image_left_channel) >= 0) {
        stereo_image_left_sub = bot_core_image_t_subscribe(lcm, stereo_image_left_channel, &stereo_image_left_handler, &hud);
    }

    char *octomap_channel;
    if (bot_param_get_str(param, "lcm_channels.octomap", &octomap_channel) >= 0) {
        octomap_sub = octomap_raw_t_subscribe(lcm, octomap_channel, &octomap_raw_t_handler, NULL);
    }

    char *stereo_bm_channel;
    if (bot_param_get_str(param, "lcm_channels.stereo_bm", &stereo_bm_channel) >= 0) {
        stereo_bm_sub = lcmt_stereo_subscribe(lcm, stereo_bm_channel, &stereo_bm_handler, NULL);
    }

    // control-c handler
    signal(SIGINT,sighandler);

    namedWindow("HUD", CV_WINDOW_AUTOSIZE | CV_WINDOW_KEEPRATIO);
    setMouseCallback("HUD", OnMouse, &hud);

    if (move_window_x != -1 && move_window_y != -1) {
        moveWindow("HUD", move_window_x, move_window_y);
    }

    cout << "Running..." << endl;

    bool change_flag = true;

    while (true) {
        // read the LCM channel, but process everything to allow us to drop frames
        while (NonBlockingLcm(lcm)) {
            change_flag = true;
        }

        if (change_flag == true || ui_box) {
            change_flag = false;

            Mat hud_image, temp_image;

            image_mutex.lock();
            left_image.copyTo(temp_image);
            image_mutex.unlock();

            // -- BM stereo -- //
            vector<Point3f> bm_points;
            stereo_bm_mutex.lock();
            if (last_stereo_bm_msg) {
                Get3DPointsFromStereoMsg(last_stereo_bm_msg, &bm_points);
            }
            stereo_bm_mutex.unlock();

            vector<int> valid_bm_points;

            if (box_bottom.x == -1) {
                Draw3DPointsOnImage(temp_image, &bm_points, stereo_calibration.M1, stereo_calibration.D1, stereo_calibration.R1, 128, 0);
            } else {
                 Draw3DPointsOnImage(temp_image, &bm_points, stereo_calibration.M1, stereo_calibration.D1, stereo_calibration.R1, 128, 0, box_top, box_bottom, &valid_bm_points);
             }


            // -- octomap -- //
            vector<Point3f> octomap_points;

            BotTrans global_to_body;
            bot_frames_get_trans(bot_frames, "local", "opencvFrame", &global_to_body);

            octomap_mutex.lock();
            if (octree != NULL) {
                StereoOctomap::GetOctomapPoints(octree, &octomap_points, &global_to_body, true);

                //Draw3DPointsOnImage(temp_image, &octomap_points, stereo_calibration.M1, stereo_calibration.D1, stereo_calibration.R1, 128);
            }
            octomap_mutex.unlock();

            // -- stereo -- //

            // transform the point from 3D space back onto the image's 2D space
            vector<Point3f> lcm_points;

            stereo_mutex.lock();
            if (last_stereo_msg) {
                Get3DPointsFromStereoMsg(last_stereo_msg, &lcm_points);
            }
            stereo_mutex.unlock();

            //cout << lcm_points << endl;

            Draw3DPointsOnImage(temp_image, &lcm_points, stereo_calibration.M1, stereo_calibration.D1, stereo_calibration.R1, 0);

            // -- octomap XY -- //
            stereo_xy_mutex.lock();
            if (last_stereo_xy_msg) {
                vector<Point> xy_points;

                Get2DPointsFromLcmXY(last_stereo_xy_msg, &xy_points);
                Draw2DPointsOnImage(temp_image, &xy_points);
            }
            stereo_xy_mutex.unlock();


            // -- box ui -- //

            // check for box_ui management
            ui_box_mutex.lock();

            if (ui_box_done) {

                // the box is done
                ui_box_done = false;
                ui_box_first_click = false;


                // write to a file, update variables, and ask for a new frame

                box_file << hud.GetVideoNumber() << "," << hud.GetFrameNumber();
                cout << hud.GetVideoNumber() << "," << hud.GetFrameNumber();

                for (int valid : valid_bm_points) {
                    box_file << "," << valid;
                    cout << "," << valid;
                }

                box_file << endl;
                cout << endl;

                // request a new frame
                AskForFrame(hud.GetVideoNumber(), hud.GetFrameNumber() + 1);

                box_top = box_bottom;
                box_bottom = Point2d(-1, -1);
            }
            ui_box_mutex.unlock();


            // remap
            Mat remapped_image;
            if (show_unremapped == false) {
                remap(temp_image, remapped_image, stereo_calibration.mx1fp, Mat(), INTER_NEAREST);
            } else {
                temp_image.copyTo(remapped_image);
            }

            if (ui_box) {
                if (box_bottom.x == -1) {
                    line(remapped_image, Point(box_top.x, 0), Point(box_top.x, remapped_image.rows), 0);
                    line(remapped_image, Point(0, box_top.y), Point(remapped_image.cols, box_top.y), 0);
                } else {
                    rectangle(remapped_image, box_top, box_bottom, 128);
                }
            }

            hud.DrawHud(remapped_image, hud_image);

            if (replay_hud_bool) {
                Mat temp;
                hud_image.copyTo(temp);
                replay_hud.DrawHud(temp, hud_image);
            }


            if (record_hud) {
                // put this frame into the HUD recording
                recording_manager.RecFrameHud(hud_image);

            }

            imshow("HUD", hud_image);
        }



        char key = waitKey(1);

        if (key != 255 && key != -1)
        {
            cout << endl << key << endl;
        }

        switch (key)
        {
            case 'q':
                sighandler(0);
                break;

            case 'R':
                record_hud = true;
                recording_manager.RestartRecHud();
                break;

            case 'c':
                hud.SetClutterLevel(hud.GetClutterLevel() + 1);
                break;

            case 'C':
                hud.SetClutterLevel(hud.GetClutterLevel() - 1);
                break;
        }
    }

    return 0;
}

/**
 * Mouse callback so that the user can click on an image
 */
void OnMouse( int event, int x, int y, int flags, void* hud_in) {

    Hud *hud = (Hud*) hud_in;

    ui_box_mutex.lock();

    if( event == EVENT_LBUTTONUP) { // left button click
        if (ui_box_first_click) {
            ui_box_done = true;
        } else {
            ui_box_first_click = true;
        }
    }

    if (ui_box_first_click) {
        box_bottom = Point2d(x / hud->GetImageScaling(), y / hud->GetImageScaling());

    } else {
        box_top = Point2d(x / hud->GetImageScaling(), y / hud->GetImageScaling());
        box_bottom = Point2d(-1, -1);
    }

    ui_box_mutex.unlock();
}

void AskForFrame(int video_number, int frame_number) {
    // construct a replay message
    lcmt_stereo msg;

    msg.timestamp = 0;
    msg.number_of_points = 0;
    msg.video_number = video_number;
    msg.frame_number = frame_number;

    lcmt_stereo_publish(lcm, "stereo_replay", &msg);
}


void sighandler(int dum)
{
    printf("\n\nclosing... ");

    if (box_file.is_open()) {
        box_file.close();
    }

    lcm_destroy (lcm);

    printf("done.\n");

    exit(0);
}



void stereo_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_stereo *msg, void *user) {
    stereo_mutex.lock();
    if (last_stereo_msg) {
        delete last_stereo_msg;
    }

    last_stereo_msg = lcmt_stereo_copy(msg);
    stereo_mutex.unlock();
}

void stereo_xy_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_stereo_with_xy *msg, void *user) {
    stereo_xy_mutex.lock();
    if (last_stereo_xy_msg) {
        delete last_stereo_xy_msg;
    }

    last_stereo_xy_msg = lcmt_stereo_with_xy_copy(msg);
    stereo_xy_mutex.unlock();
}



void stereo_bm_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_stereo *msg, void *user) {
    stereo_bm_mutex.lock();
    if (last_stereo_bm_msg) {
        delete last_stereo_bm_msg;
    }

    last_stereo_bm_msg = lcmt_stereo_copy(msg);
    stereo_bm_mutex.unlock();
}

void stereo_image_left_handler(const lcm_recv_buf_t *rbuf, const char* channel, const bot_core_image_t *msg, void *user) {

    if (msg->pixelformat != 1196444237) { // PIXEL_FORMAT_MJPEG
        cerr << "Warning: reading images other than JPEG not yet implemented." << endl;
        return;
    }

    image_mutex.lock();

    left_image = Mat::zeros(msg->height, msg->width, CV_8UC1);

    // decompress JPEG
    jpeg_decompress_8u_gray(msg->data, msg->size, left_image.data, msg->width, msg->height, left_image.step);

    image_mutex.unlock();

}

// for replaying videos, subscribe to the stereo replay channel and set the frame number
void stereo_replay_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_stereo *msg, void *user) {
    Hud *hud = (Hud*)user;

    hud->SetFrameNumber(msg->frame_number);
    hud->SetVideoNumber(msg->video_number);
}

void baro_airspeed_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_baro_airspeed *msg, void *user) {
    Hud *hud = (Hud*)user;

    hud->SetAirspeed(msg->airspeed);
}

void battery_status_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_battery_status *msg, void *user) {
    Hud *hud = (Hud*)user;

    hud->SetBatteryVoltage(msg->voltage);
}

void servo_out_handler(const lcm_recv_buf_t *rbuf, const char* channel, const lcmt_deltawing_u *msg, void *user) {

    Hud *hud = (Hud*)user;

    hud->SetServoCommands((msg->throttle - 1100) * 100/797, (msg->elevonL-1000)/10.0, (msg->elevonR-1000)/10.0);
    hud->SetAutonomous(msg->is_autonomous);
}

void mav_gps_data_t_handler(const lcm_recv_buf_t *rbuf, const char* channel, const mav_gps_data_t *msg, void *user) {
    Hud *hud = (Hud*)user;

    hud->SetGpsSpeed(msg->speed);
    hud->SetGpsHeading(msg->heading);
}

void mav_pose_t_handler(const lcm_recv_buf_t *rbuf, const char* channel, const mav_pose_t *msg, void *user) {
    Hud *hud = (Hud*)user;

    hud->SetAltitude(msg->pos[2]);
    hud->SetOrientation(msg->orientation[0], msg->orientation[1], msg->orientation[2], msg->orientation[3]);
    hud->SetAcceleration(msg->accel[0], msg->accel[1], msg->accel[2]);

    hud->SetTimestamp(msg->utime);
}


void octomap_raw_t_handler(const lcm_recv_buf_t *rbuf, const char* channel, const octomap_raw_t *msg, void *user) {
    // get an octomap and load it into memory

    octomap_mutex.lock();

    if (octree) {
        delete octree;
    }

    std::stringstream datastream;
    datastream.write((const char*) msg->data, msg->length);

    octree = new octomap::OcTree(1); //resolution will be set by data from message
    octree->readBinary(datastream);

    octree->toMaxLikelihood();

    octomap_mutex.unlock();

}

void Get2DPointsFromLcmXY(lcmt_stereo_with_xy *msg, vector<Point> *xy_points) {
    for (int i = 0; i < msg->number_of_points; i ++) {
        xy_points->push_back(Point(msg->frame_x[i], msg->frame_y[i]));
    }
}

void Draw2DPointsOnImage(Mat image, vector<Point> *points) {
    for (Point point : *points) {
        rectangle(image, Point(point.x-1, point.y-1), Point(point.x+1, point.y+1), 0);
    }
}


/**
 * Processes LCM messages without blocking.
 *
 * @param lcm lcm object
 *
 * @retval true if processed a message
 */
bool NonBlockingLcm(lcm_t *lcm)
{
    // setup an lcm function that won't block when we read it
    int lcm_fd = lcm_get_fileno(lcm);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(lcm_fd, &fds);

    // wait a limited amount of time for an incoming message
    struct timeval timeout = {
        0,  // seconds
        1   // microseconds
    };


    int status = select(lcm_fd + 1, &fds, 0, 0, &timeout);

    if(0 == status) {
        // no messages
        //do nothing
        return false;

    } else if(FD_ISSET(lcm_fd, &fds)) {
        // LCM has events ready to be processed.
        lcm_handle(lcm);
        return true;
    }
    return false;

}


