import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt
import os
from collections import OrderedDict


resfiles = []
for filename in os.listdir("./"):
    if filename.startswith("result-") and filename.endswith(".txt"):
        resfiles.append(filename)


for rp in (100, 95, 50, 0):
    for hr in (99, 95, 80):

        avg_load_admits = {}
        avg_cache_tps = {}
        avg_core_tps = {}
        avg_total_tps = {}

        for filename in resfiles:
            intensity = int(filename[filename.find("-int")+4:filename.find("-read")])
            read_percentage = int(filename[filename.find("-read")+5:filename.find("-hit")])
            hit_ratio = int(filename[filename.find("-hit")+4:filename.rfind("-")])
            mode = filename[filename.rfind("-")+1:filename.find(".txt")]

            if read_percentage != rp or hit_ratio != hr:
                continue

            if mode not in avg_load_admits:
                avg_load_admits[mode] = {}
                avg_cache_tps[mode] = {}
                avg_core_tps[mode] = {}
                avg_total_tps[mode] = {}

            num_reqs = []
            times = []
            # miss_ratios = []
            load_admits = []
            cache_tps = []
            core_tps = []

            with open(filename) as resfile:
                for line in resfile.readlines():
                    line = line.strip()

                    if line.startswith("..."):
                        num_req = int(line[line.find("#")+1:line.find(" @")])
                        time = float(line[line.find("@ ")+2:line.find(" ms")])
                        # miss_ratio = float(line[line.find("miss_ratio = ")+13:line.find(", load_admit")])
                        load_admit = float(line[line.find("load_admit = ")+13:line.find(", cache")])
                        cache_tp = float(line[line.find("cache_tp = ")+11:line.find(", core")])
                        core_tp = float(line[line.find("core_tp = ")+10:])

                        num_reqs.append(num_req)
                        times.append(time)
                        # miss_ratios.append(miss_ratio)
                        load_admits.append(load_admit)
                        cache_tps.append(cache_tp / 1024.0)     # MiB/s
                        core_tps.append(core_tp / 1024.0)       # MiB/s

            # Amplification ratio: actual number of requests may be smaller than what it
            # should be given the intensity number, because there is overhead in benchmarking
            # code loop. Doing usleep of delta (= 1 / intensity) will actually give an
            # intensity smaller than what we set.
            time_length = (times[-1] - times[0]) / 1000
            amp_ratio = float(intensity * time_length) / float(num_reqs[-1])

            # print(mode, amp_ratio)

            intensity /= 1000

            avg_load_admit = 0
            avg_cache_tp = 0
            avg_core_tp = 0

            for i in range(len(times)):
                avg_load_admit += load_admits[i]
                avg_cache_tp += cache_tps[i] * amp_ratio
                avg_core_tp += core_tps[i] * amp_ratio

            avg_load_admits[mode][intensity] = avg_load_admit / len(times)
            avg_cache_tps[mode][intensity] = avg_cache_tp / len(times)
            avg_core_tps[mode][intensity] = avg_core_tp / len(times)
            avg_total_tps[mode][intensity] = (avg_cache_tp + avg_core_tp) / len(times)

        for mode in avg_load_admits.keys():
            avg_load_admits[mode] = OrderedDict(sorted(avg_load_admits[mode].items()))
            avg_cache_tps[mode] = OrderedDict(sorted(avg_cache_tps[mode].items()))
            avg_core_tps[mode] = OrderedDict(sorted(avg_core_tps[mode].items()))
            avg_total_tps[mode] = OrderedDict(sorted(avg_total_tps[mode].items()))

        # print(avg_load_admits)
        # print(avg_cache_tps)
        # print(avg_core_tps)
        # print(avg_total_tps)

        if len(avg_load_admits) == 0:
            continue


        # for baseline in ('wa', 'wb', 'wt'):
        #     fig = plt.figure(filename, constrained_layout=True)
        #     gs = fig.add_gridspec(3, 1)

        #     ax1 = fig.add_subplot(gs[0,:])
        #     ax1.bar([v-0.25 for v in avg_cache_tps['pt'].keys()],
        #             avg_cache_tps['pt'].values(),
        #             width=0.25, color="lightsteelblue", edgecolor="white", label="pt")
        #     ax1.bar(avg_cache_tps[baseline].keys(),
        #             avg_cache_tps[baseline].values(),
        #             width=0.25, color="blue",edgecolor="white", label=baseline)
        #     ax1.bar([v+0.25 for v in avg_cache_tps['mf'+baseline].keys()],
        #             avg_cache_tps['mf'+baseline].values(),
        #             width=0.25, color="midnightblue", edgecolor="white", label="mf"+baseline)
        #     ax1.set_title("Cache Throughput")
        #     ax1.set_xticks(range(10, 25))
        #     ax1.set_ylim((0, 100.))
        #     ax1.set_ylabel("(MiB/s)")
        #     ax1.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

        #     ax2 = fig.add_subplot(gs[1,:])
        #     ax2.bar([v-0.25 for v in avg_core_tps['pt'].keys()],
        #             avg_core_tps['pt'].values(),
        #             width=0.25, color="bisque", edgecolor="white", label="pt")
        #     ax2.bar(avg_core_tps[baseline].keys(),
        #             avg_core_tps[baseline].values(),
        #             width=0.25, color="darkorange", edgecolor="white", label=baseline)
        #     ax2.bar([v+0.25 for v in avg_core_tps['mf'+baseline].keys()],
        #             avg_core_tps['mf'+baseline].values(),
        #             width=0.25, color="saddlebrown", edgecolor="white", label="mf"+baseline)
        #     ax2.set_title("Core Throughput")
        #     ax2.set_xticks(range(10, 25))
        #     ax2.set_ylim((0, 100.))
        #     ax2.set_ylabel("(MiB/s)")
        #     ax2.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

        #     ax3 = fig.add_subplot(gs[2:,:])
        #     ax3.bar([v-0.25 for v in avg_total_tps['pt'].keys()],
        #             avg_total_tps['pt'].values(),
        #             width=0.25, color="lightgreen", edgecolor="white", label="pt")
        #     ax3.bar(avg_total_tps[baseline].keys(),
        #             avg_total_tps[baseline].values(),
        #             width=0.25, color="lime", edgecolor="white", label=baseline)
        #     ax3.bar([v+0.25 for v in avg_total_tps['mf'+baseline].keys()],
        #             avg_total_tps['mf'+baseline].values(),
        #             width=0.25, color="darkgreen", edgecolor="white", label="mf"+baseline)
        #     ax3.set_title("Total Throughput")
        #     ax3.set_xticks(range(10, 25))
        #     ax3.set_ylim((0, 100.))
        #     ax3.set_ylabel("(MiB/s)")
        #     ax3.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

        #     ax3.set_xlabel("Intensity (x1000 4K-Reqs/s)")

        #     fig.suptitle("Throughput on Different Intensities, baseline = " + baseline)

        #     plt.savefig("intensity-" + baseline + ".png", dpi=200)
        #     plt.close()

        for baseline in ('wa', 'wb', 'wt'):
            fig = plt.figure(filename, constrained_layout=True)
            gs = fig.add_gridspec(3, 1)

            ax1 = fig.add_subplot(gs[0,:])
            ax1.bar([v-0.2 for v in avg_cache_tps[baseline].keys()],
                    avg_cache_tps[baseline].values(),
                    width=0.4, color="lightsteelblue", edgecolor="white", label=baseline)
            ax1.bar([v+0.2 for v in avg_cache_tps['mf'+baseline].keys()],
                    avg_cache_tps['mf'+baseline].values(),
                    width=0.4, color="midnightblue", edgecolor="white", label="mf"+baseline)
            ax1.set_title("Cache Throughput")
            ax1.set_xticks(range(10, 26))
            ax1.set_ylim((0, 100.))
            ax1.set_ylabel("(MiB/s)")
            ax1.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

            ax2 = fig.add_subplot(gs[1,:])
            ax2.bar([v-0.2 for v in avg_core_tps[baseline].keys()],
                    avg_core_tps[baseline].values(),
                    width=0.4, color="bisque", edgecolor="white", label=baseline)
            ax2.bar([v+0.2 for v in avg_core_tps['mf'+baseline].keys()],
                    avg_core_tps['mf'+baseline].values(),
                    width=0.4, color="saddlebrown", edgecolor="white", label="mf"+baseline)
            ax2.set_title("Core Throughput")
            ax2.set_xticks(range(10, 26))
            ax2.set_ylim((0, 100.))
            ax2.set_ylabel("(MiB/s)")
            ax2.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

            ax3 = fig.add_subplot(gs[2:,:])
            ax3.bar([v-0.2 for v in avg_total_tps[baseline].keys()],
                    avg_total_tps[baseline].values(),
                    width=0.4, color="lightgreen", edgecolor="white", label=baseline)
            ax3.bar([v+0.2 for v in avg_total_tps['mf'+baseline].keys()],
                    avg_total_tps['mf'+baseline].values(),
                    width=0.4, color="darkgreen", edgecolor="white", label="mf"+baseline)
            ax3.set_title("Total Throughput")
            ax3.set_xticks(range(10, 26))
            ax3.set_ylim((0, 100.))
            ax3.set_ylabel("(MiB/s)")
            ax3.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

            ax3.set_xlabel("Intensity (x1000 4K-Reqs/s)")

            fig.suptitle("Throughput on Different Intensities, baseline = " + baseline)

            plt.savefig("intensity-" + baseline + ".png", dpi=200)
            plt.close()
