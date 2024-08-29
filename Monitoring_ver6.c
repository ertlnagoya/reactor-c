/*
ファイルの読み書きによるプロセス間通信を行う
inotify系の関数を用いて、書き込みを監視する。
監視対象はファイルで、ファイルの配置場所は
あらかじめfile_pathとして指定しておく。

コマンド、通信用ファイルのパスなど先に格納

書き込み内容を読み取った後に内容を消去するように変更

並列実行に対応するように変更
・各スレッドの末端CPによって監視の開始・終了を行う

停止後の再起動機能を追加
カウントダウン（Wdg），CP受信処理（カウント更新），再起動の3つのスレッドに変更．
カウントダウン，CP受信処理を最高優先度，再起動は低優先度．
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <sys/timerfd.h>
#include <sys/select.h>
#include <signal.h>
#include <pthread.h>  //スレッド化に必要
#include <sched.h>  //スケジューリングポリシーの設定に必要


#define RTI_federate_nodes 3 //RTIを含めたfederate数
#define EVENT_BUF_LEN     (1024 * (EVENT_SIZE + 16))
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define max_cp_num 10
#define p_state_stop 0          //初回起動前の状態
#define p_state_starting 1      //起動中
#define p_state_terminated 2    //強制停止された状態


typedef struct check_point_info {
    bool check_do;         //監視実行中フラグ
    bool start_cp;         //スレッドから送信される最初のcheck point番号
    bool end_cp;           //スレッドから送信される最後のcheck point番号
    int timer_count;       //各CPのカウント値（process_info.deadline配列より抽出）
} cp_info;

typedef struct process_info {
    pid_t pid;             //Pid
    int p_state;           //プロセスの状態　実行 = 1，停止 = 0
    FILE *fd;              //Pid，CPが書かれるファイルを指すポインタ
    char file_path[256];   //通信用ファイルの配置場所指定
    char command[256];     //コマンドを格納
    char cp_num[256];      //受信したcheck pointの番号
    int deadline[10];      //デッドラインを格納する配列（各reaction単位）
    cp_info* cp_array[max_cp_num];  //各cpに関しての情報を格納
} process_info;

/*
再起動を行う関数
*/
void* restartProgram(void *arg) {
    //キャストし直し，構造体を参照できるようにする
    struct process_info *p_info = (struct process_info *)arg;

    for(int i = 0; i<RTI_federate_nodes; i++) {
        if(p_info[i].p_state == p_state_terminated) {
            int result = system(p_info[i].command);
            if (result == -1) {
                //デバッグ
                perror("Monitoring: Error executing program");
            } else {
                //デバッグ
                printf("Monitoring: Command %s executed successfully\n", p_info[i].command);

                //プロセスの状態を更新
                p_info[i].p_state = p_state_starting;
                
                //Pid取得
                p_info[i].fd = fopen(p_info[i].file_path, "r+");
                if(p_info[i].fd == NULL) {
                    perror("Monitoring: Faild make file");
                } else {
                    fscanf(p_info[i].fd, "%d", &(p_info[i].pid));
                    printf("Monitoring: scanned pid %d\n", p_info[i].pid);
                    int fd = fileno(p_info[i].fd);
                    if(ftruncate(fd, 0) != 0) {
                        perror("Monitoring: Failed to truncate file\n");
                        close(fd);
                    }
                }
                fclose(p_info[i].fd);
                printf("Monitoring: file closed\n");
            }
        }
        /*
        if(i == RTI_federate_nodes) {
            i = 0;
        }
        */
    }
}

/*
受信したCPを基に，カウント値を更新する関数
*/
void time_count_update(process_info *p_info) {
    int cp_num_value = atoi(p_info -> cp_num);
    
    if(p_info->cp_array[cp_num_value]->end_cp == true) {
        p_info->cp_array[cp_num_value - 1]->check_do = false;
    } else if(p_info->cp_array[cp_num_value]->start_cp == true) {
        p_info->cp_array[cp_num_value]->timer_count = p_info->deadline[cp_num_value];
        p_info->cp_array[cp_num_value]->check_do = true;
    } else {
        p_info->cp_array[cp_num_value]->timer_count = p_info->deadline[cp_num_value];
        p_info->cp_array[cp_num_value]->check_do = true;
        p_info->cp_array[cp_num_value - 1]->check_do = false;
    }
}

/*
タイマーイベントによるカウントダウン．カウント値0で対応プロセスを強制停止
*/
void* count_down(void *arg) {
    //キャストし直し，構造体を参照できるようにする
    struct process_info *p_info = (struct process_info *)arg;
    
    //イベント読み取りのための変数
    uint64_t expirations;
    ssize_t size;
    
    /*カウントタイマー作成*/
    int timer_fd;
    struct itimerspec timer;
    /*カウントタイマーの時間を設定．起動間隔1ms*/
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_nsec = 1000000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_nsec = 1000000;
    timer_fd = timerfd_create(CLOCK_REALTIME, 0);
    if (timer_fd == -1) {
        perror("Monitoring: timerfd_create is failed.\n");
    }

    timerfd_settime(timer_fd, 0, &timer, NULL); // タイマーイベントのセット

    while(1) {
        size = read(timer_fd, &expirations, sizeof(expirations));
        if (size != sizeof(expirations)) {
            perror("Monitoring: reading Wdg event is filed\n");
        }

        //sudoでkillする．system()を用いる
        for(int i =0; i<RTI_federate_nodes; i++) {
            for(int j = 0; j<max_cp_num; j++) {
                if(p_info[i].cp_array[j]->check_do == true && p_info[i].p_state == p_state_starting) {
                    p_info[i].cp_array[j]->timer_count--;
                    //カウント値が0であれば停止
                    if(p_info[i].cp_array[j]->timer_count == 0) {
                        char cmd[50];
                        sprintf(cmd, "sudo kill %d", p_info[i].pid);
                        printf("Monitoring: will kill PID: %d\n", p_info[i].pid);
                        
                        int result = system(cmd);
                        if(result == -1) {
                            perror("Monitoring:  system kill");
                        } else {
                            printf("Monitoring: kill success\n");
                            //プロセス状態を更新
                            p_info[i].p_state = p_state_terminated;
                        }
                    }
                }
            }
        }
    }

    close(timer_fd);
    return NULL;
}

/*
プロセスの情報を先に格納しておく（他に良い方法がありそう）
*/
void p_info_write(process_info *p_info) {
    //p_infoのメンバー変数を初期化
    for (int i = 0; i < RTI_federate_nodes; ++i) {
        // 各 process_info 構造体のメンバーを初期化
        p_info[i].pid = 0;
        p_info[i].p_state = p_state_stop;
        p_info[i].fd = NULL;
        memset(p_info[i].file_path, 0, sizeof(p_info[i].file_path));
        memset(p_info[i].command, 0, sizeof(p_info[i].command));
        memset(p_info[i].cp_num, 0, sizeof(p_info[i].cp_num));
        memset(p_info[i].deadline, 0, sizeof(p_info[i].deadline));

        // cp_array の各ポインタを NULL に初期化
        for (int j = 0; j < max_cp_num; ++j) {
            p_info[i].cp_array[j] = NULL;
        }

        // cp_info 構造体のインスタンスを動的に作成して初期化
        for (int j = 0; j < max_cp_num; ++j) {
            p_info[i].cp_array[j] = (cp_info*)malloc(sizeof(cp_info));
            if (p_info[i].cp_array[j] == NULL) {
                perror("Monitoring: Failed to allocate memory for cp_info");
            }
            p_info[i].cp_array[j]->check_do = false;
            p_info[i].cp_array[j]->start_cp = false;
            p_info[i].cp_array[j]->end_cp = false;
            p_info[i].cp_array[j]->timer_count = 0;
        }
    }

    //コマンドを格納
    strcpy(p_info[0].command, "taskset -c 0 RTI -n 2 -r 3000000000 & echo $! > /home/yoshinoriterazawa/LF/RTI.txt");
    strcpy(p_info[1].command, "taskset -c 1 /home/yoshinoriterazawa/LF/fed-gen/filewrite/bin/federate__writer & echo $! > /home/yoshinoriterazawa/LF/federate_writer.txt");
    strcpy(p_info[2].command, "taskset -c 2 /home/yoshinoriterazawa/LF/fed-gen/filewrite/bin/federate__m_writer & echo $! > /home/yoshinoriterazawa/LF/federate_m_writer.txt");
    //通信用ファイルのパスを格納
    strcpy(p_info[0].file_path, "/home/yoshinoriterazawa/LF/RTI.txt");
    strcpy(p_info[1].file_path, "/home/yoshinoriterazawa/LF/federate_writer.txt");
    strcpy(p_info[2].file_path, "/home/yoshinoriterazawa/LF/federate_m_writer.txt");

    //実行シーケンスの最初のCPを設定
    p_info[1].cp_array[0]->start_cp = true;
    //p_info[1].cp_array[4]->start_cp = true;

    p_info[2].cp_array[0]->start_cp = true;
    

    //end_cp_num（最後のCP番号）を格納（とりあえずfederateのみ）
    p_info[1].cp_array[3]->end_cp = true;
    //p_info[1].cp_array[7]->end_cp = true;

    p_info[2].cp_array[3]->end_cp = true;

    //デッドラインを格納（とりあえずfederateのみ）
    p_info[1].deadline[0] = 1010;
    p_info[1].deadline[1] = 100;
    p_info[1].deadline[2] = 1010;
    //p_info[1].deadline[4] = 1010;
    //p_info[1].deadline[5] = 100;
    //p_info[1].deadline[6] = 1010;

    p_info[2].deadline[0] = 500;
    p_info[2].deadline[1] = 100;
    p_info[2].deadline[2] = 1010;
}

/*
プログラムを実行する．その際，実行したプログラムのpidを取得
*/
void executeProgram(process_info *p_info, int wd[], int *inotify_fd) {
    //監視のためにinotifyインスタンスを生成
    if((*inotify_fd = inotify_init()) == -1) {
        perror("Monitoring: Error inotify_init");
        exit(EXIT_FAILURE);
    }
    
    for(int i =0; i < RTI_federate_nodes; i++) {
        sleep(1);
        int result = system(p_info[i].command);
        if (result == -1) {
            //デバッグ
            perror("Monitoring: Error executing program");
        } else {
            //デバッグ
            printf("Monitoring: Command %s executed successfully\n", p_info[i].command);

            //プロセスの状態を更新
            p_info[i].p_state = p_state_starting;
            
            //Pid取得
            p_info[i].fd = fopen(p_info[i].file_path, "r+");
            if(p_info[i].fd == NULL) {
                perror("Monitoring: Faild make file");
            } else {
                fscanf(p_info[i].fd, "%d", &(p_info[i].pid));
                printf("Monitoring: scanned pid %d\n", p_info[i].pid);
                int fd = fileno(p_info[i].fd);
                if(ftruncate(fd, 0) != 0) {
                    perror("Monitoring: Failed to truncate file\n");
                    close(fd);
                }
            }
            fclose(p_info[i].fd);
            printf("Monitoring: file closed\n");
            

            //通信用ファイルを監視対象に設定
            wd[i] = inotify_add_watch(*inotify_fd, p_info[i].file_path, IN_MODIFY);
            if(wd[i] == -1) {
                perror("Monitoring: Error inotify_add_watch");
                exit(EXIT_FAILURE);
            } else {
                printf("Monitoring: 監視対象設定完了\n");
            }
        }
    }
}

/*
CPの書き込みまたはタイマーイベントの起動を待ち、読み込む関数
*/
void watch_cp_write(process_info *p_info, int wd[], int *inotify_fd) {
    char buffer[EVENT_BUF_LEN]; //イベント格納バッファ
    char line[256]; //通信用ファイルを1行ずつ読み込むための配列
    
    while (1) {
        //変更イベントを読み取る。readを使うことで一度に読み取り
        int length = read(*inotify_fd, buffer, EVENT_BUF_LEN);  //lengthはバイト数が入る。readで一度に読み取り
        if (length < 0) {
            perror("Monitoring: read event failed");
            exit(EXIT_FAILURE);
        }

        //読み込んだ変更イベントを1つずつ処理する
        // event_point : 変更内容を順に取得するために使用．event毎の先頭アドレスを指す
        int event_point = 0;
        while (event_point < length) {
            struct inotify_event *event = (struct inotify_event *) &buffer[event_point];  //キャストすることで、それぞれのイベントの先頭アドレスを指定
            // 変更のみ対応
            if (event->mask & IN_MODIFY) {
                // 変更されたファイルを探す
                for(int i = 0; i< RTI_federate_nodes; i++) {
                    if(event->wd == wd[i]) {
                        p_info[i].fd = fopen(p_info[i].file_path, "r+");
                        if (!p_info[i].fd) {
                            perror("Monitoring: Error opening file");
                            continue;
                        }

                        while (flock(fileno(p_info[i].fd), LOCK_EX) == -1) { // 排他ロックを取得
                            perror("Monitoring: Failed to lock file");
                            usleep(1000); // 待機してリトライ
                        }

                        int last_line = 0;
                        while (fgets(line, sizeof(line), p_info[i].fd) != NULL) {
                            last_line = 1;
                            sscanf(line, "cp: %s", p_info[i].cp_num);
                            printf("Monitoring: CP_num %s\n", p_info[i].cp_num);

                            //CP受信による実行時間監視の開始
                            time_count_update(&p_info[i]);
                            //実行デバック
                            int cp_num_value = atoi(p_info[i].cp_num);
                            printf("Monitoring: updated count %d\n", p_info[i].cp_array[cp_num_value]->timer_count);
                        }

                        if (last_line) {
                            int fd = fileno(p_info[i].fd);
                            if (ftruncate(fd, 0) != 0) {
                                perror("Monitoring: Failed to truncate file\n");
                            }
                            rewind(p_info[i].fd); // ファイルポインタを先頭に戻す
                        }

                        fclose(p_info[i].fd);
                    }
                }
                event_point += EVENT_SIZE + event->len;
            }
        }
        restartProgram(p_info);
    }
}


int main() {
    pthread_t reconnect_thread, Wdg_thread;
    struct sched_param param;
    int policy = SCHED_FIFO;

    process_info p_info[RTI_federate_nodes]; //RTI, federateの情報を格納する構造体配列の作成
    int wd[RTI_federate_nodes]; //RTI, federateを監視するためのウォッチディスクリプタ配列
    int inotify_fd;  //inotifyインスタンスのファイルディスクリプタ

    //メインスレッドのschedポリシー，優先度を設定
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), policy, &param);

    //必要情報を先に記録する
    p_info_write(p_info);

    //Wdgをスレッドとして実行
    pthread_create(&Wdg_thread, NULL, count_down, (void *)&p_info);
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(Wdg_thread, policy, &param);

    //再起動用のスレッドを作成
    pthread_create(&reconnect_thread, NULL, restartProgram, (void *)&p_info);
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
    pthread_setschedparam(reconnect_thread, policy, &param);

    //プログラムを実行する．その際，実行したプログラムのpidを取得
    executeProgram(p_info, wd, &inotify_fd);

    //CPの書き込みまたはタイマーイベントの起動を待ち、読み込む関数
    watch_cp_write(p_info, wd, &inotify_fd);

    return 0;
}