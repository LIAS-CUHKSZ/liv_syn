#include "MvCameraControl.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cv_bridge/cv_bridge.h>
#include <fcntl.h>
#include <image_transport/image_transport.h>
#include <mutex>
#include <opencv2/core/hal/interface.h>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <ros/console.h>
#include <ros/ros.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

using namespace std;

image_transport::Publisher pub; // 发布图像

struct my_t {
    int64_t gps_tt;
    std::chrono::high_resolution_clock::time_point pc_tt;
};

my_t *pointt; // 共享内存指针,用于实现时间戳同步

pthread_t nThreadID; // 采集线程ID
bool exit_flag = false;

void *handle = NULL; // 相机句柄

std::string ExposureAutoStr[3] = {"Off", "Once", "Continues"};
std::string GammaSlectorStr[3] = {"User", "sRGB", "Off"};
std::string GainAutoStr[3] = {"Off", "Once", "Continues"};
std::string CameraName;
float resize_divider;

// for time sync
int pps_comp = 0;                // 补偿串口传输延迟,ns
int exp_cam = 0;                 // 补偿相机曝光延迟,ns
int trans_cam = 0;               // 相机采集和传输延迟,ns
int frame_cnt = 0;               // cam总帧数
bool update_flag = false;        // gps时间是否更新
bool first_aligned_flag = false; // 是否完成第一次对齐
int lost_cnt = 0;                // cam的丢帧数
int per_PPS_cnt = 0;             // 当前PPS之后已经收到的帧数
long align_thre = 45000000; // 用于判断gps和cam是否足够接近,pc time,ns
int64_t gps_t = 0;          // gps绝对时间,ns since epoch
std::mutex mu;
// 上一帧相机采集pc time和gps time
long gps_ns, last_frame_ns;

void signalHandler(int signum) {

    ROS_INFO("Interrupt signal (%d) received.\n", signum);
    // shutdown thread
    pthread_cancel(nThreadID);
    // Stop grabbing
    int nRet;
    nRet = MV_CC_StopGrabbing(handle);
    if (MV_OK != nRet) {
        ROS_ERROR("MV_CC_StopGrabbing fail! nRet [%x]\n", nRet);
    }

    nRet = MV_CC_CloseDevice(handle);
    if (MV_OK != nRet) {
        ROS_ERROR("MV_CC_CloseDevice fail! nRet [%x]\n", nRet);
    }

    nRet = MV_CC_DestroyHandle(handle);
    if (MV_OK != nRet) {
        ROS_ERROR("MV_CC_DestroyHandle fail! nRet [%x]\n", nRet);
    }

    ros::shutdown();
    munmap(pointt, sizeof(my_t));
    exit(signum);
}

/**
 * @brief thread for update GPS timestamp
 * TODO:换成消息队列以提高性能
 */
void queryTimeLoop() {
    // 高频率查看是否收到新的GPS时间戳
    ROS_INFO("init query");
    while (true) {
        if (mu.try_lock()) {
            if (gps_t != pointt->gps_tt) {
                // 更新gps接收时的pc时间,cast to ns
                ROS_WARN("time update!!!");
                gps_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             pointt->pc_tt.time_since_epoch())
                             .count();
                gps_ns -= pps_comp; // 补偿串口传输延迟和进程通信/调度延迟
                // 更新gps时间戳
                gps_t = pointt->gps_tt; // utc time
                ROS_INFO("update %ld", gps_t);

                update_flag = true;

                first_aligned_flag = true;
                mu.unlock();
                // 等待下一秒
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
            mu.unlock();
        }
        // ROS_INFO("nothing changes, try later");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void setParams(void *handle, const std::string &params_file) {
    cv::FileStorage Params(params_file, cv::FileStorage::READ);
    if (!Params.isOpened()) {
        string msg = "Failed to open settings file at:" + params_file;
        ROS_ERROR_STREAM(msg.c_str());
        exit(-1);
    }
    resize_divider = Params["resize_divide"];
    if (resize_divider < 0.1) {
        resize_divider = 1;
    }
    int ExposureAuto = Params["ExposureAuto"];
    int ExposureTimeLower = Params["AutoExposureTimeLower"];
    int ExposureTimeUpper = Params["AutoExposureTimeUpper"];
    int ExposureTime = Params["ExposureTime"];
    int ExposureAutoMode = Params["ExposureAutoMode"];
    int GainAuto = Params["GainAuto"];
    float Gain = Params["Gain"];
    float Gamma = Params["Gamma"];
    int GammaSlector = Params["GammaSelector"];

    int CenterAlign = Params["CenterAlign"];
    int nRet;

    // 设置曝光模式
    nRet = MV_CC_SetExposureAutoMode(handle, ExposureAutoMode);
    if (MV_OK == nRet) {
        std::string msg =
            "Set ExposureAutoMode: " + std::to_string(ExposureAutoMode);
        ROS_INFO_STREAM(msg.c_str());
    } else {
        ROS_ERROR_STREAM("Fail to set ExposureAutoMode");
    }
    // 如果是自动曝光
    if (ExposureAutoMode == 2) {
        // nRet = MV_CC_SetFloatValue(handle, "ExposureAuto", ExposureAuto);
        nRet = MV_CC_SetEnumValue(handle, "ExposureAuto",
                                  MV_EXPOSURE_AUTO_MODE_CONTINUOUS);
        if (MV_OK == nRet) {
            std::string msg =
                "Set Exposure Auto: " + ExposureAutoStr[ExposureAuto];
            ROS_INFO_STREAM(msg.c_str());
        } else {
            ROS_ERROR_STREAM("Fail to set Exposure auto mode");
        }
        nRet = MV_CC_SetAutoExposureTimeLower(handle, ExposureTimeLower);
        if (MV_OK == nRet) {
            std::string msg = "Set Exposure Time Lower: " +
                              std::to_string(ExposureTimeLower) + "ms";
            ROS_INFO_STREAM(msg.c_str());
        } else {
            ROS_ERROR_STREAM("Fail to set Exposure Time Lower");
        }
        nRet = MV_CC_SetAutoExposureTimeUpper(handle, ExposureTimeUpper);
        if (MV_OK == nRet) {
            std::string msg = "Set Exposure Time Upper: " +
                              std::to_string(ExposureTimeUpper) + "ms";
            ROS_INFO_STREAM(msg.c_str());
        } else {
            ROS_ERROR_STREAM("Fail to set Exposure Time Upper");
        }
    }
    // 如果是固定曝光
    if (ExposureAutoMode == 0) {
        nRet = MV_CC_SetExposureTime(handle, ExposureTime);
        if (MV_OK == nRet) {
            std::string msg =
                "Set Exposure Time: " + std::to_string(ExposureTime) + "ms";
            ROS_INFO_STREAM(msg.c_str());
        } else {
            ROS_ERROR_STREAM("Fail to set Exposure Time");
        }
    }

    nRet = MV_CC_SetEnumValue(handle, "GainAuto", GainAuto);

    if (MV_OK == nRet) {
        std::string msg = "Set Gain Auto: " + GainAutoStr[GainAuto];
        ROS_INFO_STREAM(msg.c_str());
    } else {
        ROS_ERROR_STREAM("Fail to set Gain auto mode");
    }

    if (GainAuto == 0) {
        nRet = MV_CC_SetGain(handle, Gain);
        if (MV_OK == nRet) {
            std::string msg = "Set Gain: " + std::to_string(Gain);
            ROS_INFO_STREAM(msg.c_str());
        } else {
            ROS_ERROR_STREAM("Fail to set Gain");
        }
    }

    nRet = MV_CC_SetGammaSelector(handle, GammaSlector);
    if (MV_OK == nRet) {
        std::string msg = "Set GammaSlector: " + GammaSlectorStr[GammaSlector];
        ROS_INFO_STREAM(msg.c_str());
    } else {
        ROS_ERROR_STREAM("Fail to set GammaSlector");
    }

    nRet = MV_CC_SetGamma(handle, Gamma);
    if (MV_OK == nRet) {
        std::string msg = "Set Gamma: " + std::to_string(Gamma);
        ROS_INFO_STREAM(msg.c_str());
    } else {
        ROS_ERROR_STREAM("Fail to set Gamma");
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    ros::init(argc, argv, "mvs_trigger");
    std::string params_file = std::string(argv[1]);
    ros::NodeHandle nh;
    image_transport::ImageTransport it(nh);
    int nRet = MV_OK;

    ros::Rate loop_rate(10);
    cv::FileStorage Params(params_file, cv::FileStorage::READ);
    if (!Params.isOpened()) {
        string msg = "Failed to open settings file at:" + params_file;
        ROS_ERROR_STREAM(msg.c_str());
        exit(-1);
    }
    std::string expect_serial_number = Params["SerialNumber"];
    std::string pub_topic = Params["TopicName"];
    std::string CameraName = Params["CameraName"];
    std::string path_for_time_stamp = Params["path_for_time_stamp"];

    pub = it.advertise(pub_topic, 1);

    int fd = open("/home/beta/tt", O_CREAT | O_RDWR, 0777);
    if (fd == -1) {
        ROS_ERROR("failed to open share mm");
    }
    pointt = (my_t *)mmap(NULL, sizeof(my_t), PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);

    MV_CC_DEVICE_INFO_LIST stDeviceList;
    memset(&stDeviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    // 枚举检测到的相机数量
    nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &stDeviceList);
    if (MV_OK != nRet) {
        ROS_ERROR("Enum Devices fail!");
    }
    int tryout = 0;
    ROS_INFO("enum end, try connect");
    while (ros::ok()) {
        if (tryout > 10) {
            ROS_WARN("fail to find any camera. Shutdown.");
            break;
        }

        bool find_expect_camera = false;
        int expect_camera_index = 0;
        if (stDeviceList.nDeviceNum == 0) {
            ROS_WARN("No Camera scaned. Retrying . . . . .\n");
            tryout++;
            ros::Duration(1).sleep();
            continue;
        } else {
            // 根据serial number启动指定相机
            for (int i = 0; i < stDeviceList.nDeviceNum; i++) {
                std::string serial_number =
                    std::string((char *)stDeviceList.pDeviceInfo[i]
                                    ->SpecialInfo.stUsb3VInfo.chSerialNumber);
                if (expect_serial_number == serial_number) {
                    ROS_INFO("found corresponding serial!");
                    find_expect_camera = true;
                    expect_camera_index = i;
                    break;
                }
            }
        }
        if (!find_expect_camera) {
            std::string msg = "Can not find the camera with serial number " +
                              expect_serial_number;
            ROS_ERROR_STREAM(msg.c_str());
            break;
        }

        nRet = MV_CC_CreateHandle(
            &handle, stDeviceList.pDeviceInfo[expect_camera_index]);
        if (MV_OK != nRet) {
            ROS_ERROR_STREAM("Create Handle fail");
            break;
        }

        nRet = MV_CC_OpenDevice(handle);
        if (MV_OK != nRet) {
            ROS_ERROR("Open Device fail\n");
            break;
        }

        // 关闭帧率控制,使用硬件触发
        nRet = MV_CC_SetBoolValue(handle, "AcquisitionFrameRateEnable", false);
        if (MV_OK != nRet) {
            ROS_ERROR("set AcquisitionFrameRateEnable fail! nRet [%x]\n", nRet);
            break;
        }

        // 获取图像大小
        MVCC_INTVALUE stParam;
        memset(&stParam, 0, sizeof(MVCC_INTVALUE));
        nRet = MV_CC_GetIntValue(handle, "PayloadSize", &stParam);
        if (MV_OK != nRet) {
            ROS_ERROR("Get PayloadSize fail! nRet [0x%x]\n", nRet);
            break;
        }

        nRet = MV_CC_SetEnumValue(handle, "PixelFormat", 0x02180014);
        if (nRet != MV_OK) {
            ROS_ERROR("Pixel setting can't work.");
            break;
        }

        // 设置各种采集参数
        ROS_INFO("setting params....");
        setParams(handle, params_file);

        // 设置触发模式为on
        // set trigger mode as on
        nRet = MV_CC_SetEnumValue(handle, "TriggerMode", 1);
        if (MV_OK != nRet) {
            ROS_ERROR("MV_CC_SetTriggerMode fail! nRet [%x]\n", nRet);
            break;
        }

        // 设置触发源
        // set trigger source
        nRet = MV_CC_SetEnumValue(handle, "TriggerSource",
                                  MV_TRIGGER_SOURCE_LINE0);
        if (MV_OK != nRet) {
            ROS_ERROR("MV_CC_SetTriggerSource fail! nRet [%x]\n", nRet);
            break;
        }

        // 开始采集
        ROS_INFO("Finish all params set! Start grabbing...");
        nRet = MV_CC_StartGrabbing(handle);
        if (MV_OK != nRet) {
            ROS_ERROR("Start Grabbing fail.\n");
            break;
        }

        // 用于保存采集到的图像
        MV_FRAME_OUT_INFO_EX stImageInfo = {0};
        memset(&stImageInfo, 0, sizeof(MV_FRAME_OUT_INFO_EX));
        unsigned char *pData =
            (unsigned char *)malloc(sizeof(unsigned char) * stParam.nCurValue);
        if (NULL == pData) {
            ROS_ERROR_STREAM("alloc memory for img data failed.");
            break;
        }

        // create a thread to update gps time stamp
        // pthread_create(&nThreadID, NULL, queryTimeLoop, NULL);
        ROS_INFO("create time quering loop...");
        std::thread t(queryTimeLoop);
        // t.join();
        ROS_INFO("wariting first align...");
        // 等待第一次时间消息到来
        while (true) {
            if (mu.try_lock()) {
                if (first_aligned_flag) {
                    ROS_INFO("begin acquisition!");
                    mu.unlock();
                    break;
                }
                mu.unlock();
            }
            ROS_INFO("waiting lidar reready");
            ros::Duration(0.01).sleep();
        }

        /*
         *  采集图像的循环, core code begins here
         */
        while (ros::ok()) {
            // 尝试采集一帧图像
            nRet = MV_CC_GetOneFrameTimeout(handle, pData, stParam.nCurValue,
                                            &stImageInfo, 1000);
            // 上升沿触发
            if (nRet == MV_OK) {
                // ROS_INFO("geet one frame");
                // 记录新帧pc时间, record the pc time ns for the new frame
                auto this_frame_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now()
                            .time_since_epoch())
                        .count();
                // 补偿相机曝光时间和传输延迟
                this_frame_ns -= (trans_cam + exp_cam);

                mu.lock(); // 上锁
                long last_diff_a = last_frame_ns - gps_ns;
                long this_diff = this_frame_ns - gps_ns;
                long this_diff_a = abs(this_diff);
                // ROS_ERROR("between two frame time %ld",
                // this_frame_ns-last_frame_ns);

                // 如果发现GPS时间发生了更新
                if (update_flag) {
                    // compare the difference between new_gps_clk and
                    // last_frame_clk/this_frame_clk, find the closest one
                    // 分别计算当前帧和上一帧与gps时间的差值，找到最近的那个
                    int incre = 0;
                    bool alinged_flag = false;
                    if (last_diff_a <= this_diff_a) {
                        // last frame is the aligned frame
                        if (last_diff_a < align_thre) {
                            incre = 1; // 现在是当前帧对齐之后的第二帧
                            alinged_flag = true;
                        }
                    } else if (last_diff_a > this_diff_a)
                        // this is first frame after aligned
                        if (this_diff_a < align_thre) {
                            incre = 0; // 对齐之后的第一帧
                            alinged_flag = true;
                        }

                    // 如果两帧都距离GPS的pc接收时间不够近,则说明相机丢帧
                    if (!alinged_flag) {
                        // use (this_frame_clk - gps_clk) to calc increment
                        // 0.05 0.1  10Hz的情况 四舍五入计算incre
                        incre = (this_diff + 50000000) / 100000000;
                        ROS_WARN_STREAM("[camera] Lost frame when PPS fired "
                                        << incre << " frames.");
                        lost_cnt += incre;
                    }
                    per_PPS_cnt = incre + 1; // 当前PPS之后已经收到的帧数
                    update_flag = false;
                } else // 若最近没有发生gps更新
                {
                    // 通过this_diff 计算这是pps内的第几帧, 四舍五入
                    int64_t num = (this_diff + 50000000) / 100000000 + 1;
                    // 丢帧判断
                    if (num - per_PPS_cnt > 1) {
                        ROS_WARN_STREAM("[camera] Lost frame between PPS"
                                        << num - per_PPS_cnt << " frames.");
                    } else {
                        per_PPS_cnt++;
                    }
                }

                // 连续丢帧超过12帧,则认为PPS出现丢失
                if (per_PPS_cnt > 12) {
                    ROS_WARN_STREAM("[GPRMC] GPS PPS lost");
                }

                // 通过(per_PPS_cnt+gps_time_stamp)计算当前帧的时间戳
                // 同时补偿半曝光时长
                double pub_t_cam = // 0.1*per_PPS_cnt. 10Hz
                    per_PPS_cnt * 100000000.0 + gps_t + (double)exp_cam / 2;

                mu.unlock(); // 解锁

                ros::Time rcv_time = ros::Time(pub_t_cam / 1000000000);
                std::string debug_msg =
                    CameraName + " GetOneFrame,nFrameNum[" +
                    std::to_string(stImageInfo.nFrameNum) +
                    "], FrameTime:" + std::to_string(rcv_time.toSec());
                // ROS_INFO_STREAM(debug_msg.c_str());
                cv::Mat srcImage;
                srcImage = cv::Mat(stImageInfo.nHeight, stImageInfo.nWidth,
                                   CV_8UC3, pData);
                cv::resize(srcImage, srcImage,
                           cv::Size(resize_divider * srcImage.cols,
                                    resize_divider * srcImage.rows),
                           CV_INTER_LINEAR);

                sensor_msgs::ImagePtr msg =
                    cv_bridge::CvImage(std_msgs::Header(), "rgb8", srcImage)
                        .toImageMsg();
                msg->header.stamp = rcv_time;
                pub.publish(msg);

                last_frame_ns = this_frame_ns;
            } else { // sleep for a while
                ros::Duration(0.001).sleep();
            } // process one frame

        } // end of acquisition inf loop

    } // end of init loop
    ROS_ERROR("fail to init camera. Shutdown.");
    return 0;
}
