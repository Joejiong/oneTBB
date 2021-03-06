/*
    Copyright (c) 2005-2020 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/*
    The original source for this example is
    Copyright (c) 1994-2008 John E. Stone
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. The name of the author may not be used to endorse or promote products
       derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
    OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
*/

#include "machine.hpp"
#include "types.hpp"
#include "macros.hpp"
#include "vector.hpp"
#include "tgafile.hpp"
#include "trace.hpp"
#include "light.hpp"
#include "shade.hpp"
#include "camera.hpp"
#include "util.hpp"
#include "intersect.hpp"
#include "global.hpp"
#include "ui.hpp"
#include "tachyon_video.hpp"

// shared but read-only so could be private too
static thr_parms *all_parms;
static scenedef scene;
static int startx;
static int stopx;
static int starty;
static int stopy;
static flt jitterscale;
static int totaly;
static int nthreads;

static int grain_size = 50;
const int DIVFACTOR = 2;

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#ifdef _WIN32
#include <windows.h>
#include "pthread_w.hpp"
#else
#include <pthread.h>
#endif

static pthread_mutex_t MyMutex, MyMutex2, MyMutex3;

static color_t render_one_pixel(int x,
                                int y,
                                unsigned int *local_mbox,
                                unsigned int &serial,
                                int startx,
                                int stopx,
                                int starty,
                                int stopy) {
    /* private vars moved inside loop */
    ray primary, sample;
    color col, avcol;
    int R, G, B;
    intersectstruct local_intersections;
    int alias;
    /* end private */

    primary = camray(&scene, x, y);
    primary.intstruct = &local_intersections;
    primary.flags = RT_RAY_REGULAR;

    serial++;
    primary.serial = serial;
    primary.mbox = local_mbox;
    primary.maxdist = FHUGE;
    primary.scene = &scene;
    col = trace(&primary);

    serial = primary.serial;

    /* perform antialiasing if enabled.. */
    if (scene.antialiasing > 0) {
        for (alias = 0; alias < scene.antialiasing; alias++) {
            serial++; /* increment serial number */
            sample = primary; /* copy the regular primary ray to start with */
            sample.serial = serial;

            {
                pthread_mutex_lock(&MyMutex);
                sample.d.x += ((rand() % 100) - 50) / jitterscale;
                sample.d.y += ((rand() % 100) - 50) / jitterscale;
                sample.d.z += ((rand() % 100) - 50) / jitterscale;
                pthread_mutex_unlock(&MyMutex);
            }

            avcol = trace(&sample);

            serial = sample.serial; /* update our overall serial # */

            col.r += avcol.r;
            col.g += avcol.g;
            col.b += avcol.b;
        }

        col.r /= (scene.antialiasing + 1.0);
        col.g /= (scene.antialiasing + 1.0);
        col.b /= (scene.antialiasing + 1.0);
    }

    /* Handle overexposure and underexposure here... */
    R = (int)(col.r * 255);
    if (R > 255)
        R = 255;
    else if (R < 0)
        R = 0;

    G = (int)(col.g * 255);
    if (G > 255)
        G = 255;
    else if (G < 0)
        G = 0;

    B = (int)(col.b * 255);
    if (B > 255)
        B = 255;
    else if (B < 0)
        B = 0;

    return video->get_color(R, G, B);
}

// need this so threads can self-schedule work; returns true (and bounds of work) if more work to do

typedef struct work_queue_entry_s {
    patch pch;
    struct work_queue_entry_s *next;
} work_queue_entry_t;
static work_queue_entry_t *work_queue_head = nullptr;
static work_queue_entry_t *work_queue_tail = nullptr;

static void generate_work(patch *pchin) {
    int startx, stopx, starty, stopy;
    int xs, ys;

    startx = pchin->startx;
    stopx = pchin->stopx;
    starty = pchin->starty;
    stopy = pchin->stopy;

    if (((stopx - startx) >= grain_size) || ((stopy - starty) >= grain_size)) {
        int xpatchsize = (stopx - startx) / DIVFACTOR + 1;
        int ypatchsize = (stopy - starty) / DIVFACTOR + 1;
        for (ys = starty; ys <= stopy; ys += ypatchsize)
            for (xs = startx; xs <= stopx; xs += xpatchsize) {
                patch pch;
                pch.startx = xs;
                pch.starty = ys;
                pch.stopx = MIN(xs + xpatchsize, stopx);
                pch.stopy = MIN(ys + ypatchsize, stopy);

                generate_work(&pch);
            }
    }
    else {
        /* just trace this patch */
        work_queue_entry_t *q = (work_queue_entry_t *)malloc(sizeof(work_queue_entry_t));
        q->pch.starty = starty;
        q->pch.stopy = stopy;
        q->pch.startx = startx;
        q->pch.stopx = stopx;
        q->next = nullptr;
        if (work_queue_head == nullptr) {
            work_queue_head = q;
        }
        else {
            work_queue_tail->next = q;
        }
        work_queue_tail = q;
    }
}

static void generate_worklist(void) {
    patch pch;
    pch.startx = startx;
    pch.stopx = stopx;
    pch.starty = starty;
    pch.stopy = stopy;
    generate_work(&pch);
}

static bool schedule_thread_work(patch &pch) {
    pthread_mutex_lock(&MyMutex3);
    work_queue_entry_t *q = work_queue_head;
    if (q != nullptr) {
        pch = q->pch;
        work_queue_head = work_queue_head->next;
    }
    pthread_mutex_unlock(&MyMutex3);
    return (q != nullptr);
}

static void parallel_thread(void *arg) {
    // thread-local storage
    unsigned int serial = 1;
    unsigned int mboxsize = sizeof(unsigned int) * (max_objectid() + 20);
    unsigned int *local_mbox = (unsigned int *)alloca(mboxsize);
    memset(local_mbox, 0, mboxsize);

    // int thread_no = (int) arg;
    patch pch;
    while (schedule_thread_work(pch)) {
        {
            drawing_area drawing(
                pch.startx, totaly - pch.stopy, pch.stopx - pch.startx, pch.stopy - pch.starty);
            for (int i = 1, y = pch.starty; y < pch.stopy; ++y, i++) {
                drawing.set_pos(0, drawing.size_y - i);
                for (int x = pch.startx; x < pch.stopx; x++) {
                    color_t c =
                        render_one_pixel(x, y, local_mbox, serial, startx, stopx, starty, stopy);
                    drawing.put_pixel(c);
                }
            }
        }
        if (!video->next_frame())
            pthread_exit(arg);
    }
    pthread_exit(arg);
}

// need this (for each platform) so we can create the right number of threads, to work efficiently

#if defined(_WIN32)

static int get_num_cpus(void) {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#elif defined(__APPLE__)

#include "sys/types.hpp"
#include "sys/sysctl.hpp"
static int get_num_cpus(void) {
    int name[2] = { CTL_HW, HW_NCPU };
    int ncpu;
    std::size_t size = sizeof(ncpu);
    sysctl(name, 2, &ncpu, &size, nullptr, 0);
    return ncpu;
}

#else /*  Linux  */

#include <sys/sysinfo.h>
static int get_num_cpus(void) {
    return get_nprocs();
}

#endif

void *thread_trace(thr_parms *parms) {
    // shared but read-only so could be private too
    all_parms = parms;
    scene = parms->scene;
    startx = parms->startx;
    stopx = parms->stopx;
    starty = parms->starty;
    stopy = parms->stopy;
    jitterscale = 40.0 * (scene.hres + scene.vres);
    totaly = parms->scene.vres;

    int n;
    nthreads = get_num_cpus();
    char *nthreads_str = getenv("THR_NUM_THREADS");
    if (nthreads_str && (sscanf(nthreads_str, "%d", &n) > 0) && (n > 0))
        nthreads = n;
    char *grain_str = getenv("THR_GRAINSIZE");
    if (grain_str && (sscanf(grain_str, "%d", &n) > 0) && (n > 0))
        grain_size = n;
    pthread_t *threads = (pthread_t *)alloca(nthreads * sizeof(pthread_t));
    pthread_mutex_init(&MyMutex, nullptr);
    pthread_mutex_init(&MyMutex2, nullptr);
    pthread_mutex_init(&MyMutex3, nullptr);
    generate_worklist(); // initialize schedule_thread_work() self-scheduler
    for (int i = 0; i < nthreads; i++) {
        pthread_create(
            &threads[i], nullptr, (void *(*)(void *))parallel_thread, (void *)((std::size_t)i));
    }
    for (int i = 0; i < nthreads; i++) {
        void *exit_val;
        pthread_join(threads[i], &exit_val);
        // expect i = (int) exit_val
    }

    return (nullptr);
}
