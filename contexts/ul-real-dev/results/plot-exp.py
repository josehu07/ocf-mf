import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt
import os
from collections import OrderedDict


resfiles = []
for filename in os.listdir("./"):
    if filename.startswith("result-") and filename.endswith(".txt"):
        resfiles.append(filename)


avg_load_admits = {'mf': {}, 'wa': {}, 'pt': {}}
avg_cache_tps = {'mf': {}, 'wa': {}, 'pt': {}}
avg_core_tps = {'mf': {}, 'wa': {}, 'pt': {}}
avg_total_tps = {'mf': {}, 'wa': {}, 'pt': {}}

for filename in resfiles:
    intensity = int(filename[filename.find("-")+1:filename.rfind("-")])
    mode = filename[filename.rfind("-")+1:filename.find(".txt")]

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

    idx_begin, idx_end = None, None

    for i in range(len(times)):
        if idx_begin is None and times[i] > (60 * 1000):
            idx_begin = i
        if idx_end is None and times[i] >= (160 * 1000):
            idx_end = i

    # print(idx_begin, idx_end, len(times))

    # Amplification ratio: actual number of requests may be smaller than what it
    # should be given the intensity number, because there is overhead in benchmarking
    # code loop. Doing usleep of delta (= 1 / intensity) will actually give an
    # intensity smaller than what we set.
    amp_ratio = float(intensity * (160 - 60)) / float(num_reqs[-1])

    # print(mode, amp_ratio)

    intensity /= 1000

    avg_load_admit = 0
    avg_cache_tp = 0
    avg_core_tp = 0

    for i in range(idx_begin, idx_end):
        avg_load_admit += load_admits[i]
        avg_cache_tp += cache_tps[i] * amp_ratio
        avg_core_tp += core_tps[i] * amp_ratio

    avg_load_admits[mode][intensity] = avg_load_admit / (idx_end - idx_begin)
    avg_cache_tps[mode][intensity] = avg_cache_tp / (idx_end - idx_begin)
    avg_core_tps[mode][intensity] = avg_core_tp / (idx_end - idx_begin)
    avg_total_tps[mode][intensity] = (avg_cache_tp + avg_core_tp) / (idx_end - idx_begin)

for mode in avg_load_admits.keys():
    avg_load_admits[mode] = OrderedDict(sorted(avg_load_admits[mode].items()))
    avg_cache_tps[mode] = OrderedDict(sorted(avg_cache_tps[mode].items()))
    avg_core_tps[mode] = OrderedDict(sorted(avg_core_tps[mode].items()))
    avg_total_tps[mode] = OrderedDict(sorted(avg_total_tps[mode].items()))

# print(avg_load_admits)
# print(avg_cache_tps)
# print(avg_core_tps)
# print(avg_total_tps)


fig = plt.figure(filename, constrained_layout=True)
gs = fig.add_gridspec(3, 1)

ax1 = fig.add_subplot(gs[0,:])
ax1.bar([v-0.25 for v in avg_cache_tps['pt'].keys()], avg_cache_tps['pt'].values(), width=0.25, color="lightsteelblue", edgecolor="white", label="PT")
ax1.bar([v      for v in avg_cache_tps['wa'].keys()], avg_cache_tps['wa'].values(), width=0.25, color="blue",           edgecolor="white", label="WA")
ax1.bar([v+0.25 for v in avg_cache_tps['mf'].keys()], avg_cache_tps['mf'].values(), width=0.25, color="midnightblue",   edgecolor="white", label="MF")
ax1.set_title("Cache Throughput")
ax1.set_xticks(range(10, 25))
ax1.set_ylim((0, 100.))
ax1.set_ylabel("(MiB/s)")
ax1.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax2 = fig.add_subplot(gs[1,:])
ax2.bar([v-0.25 for v in avg_core_tps['pt'].keys()], avg_core_tps['pt'].values(), width=0.25, color="bisque",      edgecolor="white", label="PT")
ax2.bar([v      for v in avg_core_tps['wa'].keys()], avg_core_tps['wa'].values(), width=0.25, color="darkorange",  edgecolor="white", label="WA")
ax2.bar([v+0.25 for v in avg_core_tps['mf'].keys()], avg_core_tps['mf'].values(), width=0.25, color="saddlebrown", edgecolor="white", label="MF")
ax2.set_title("Core Throughput")
ax2.set_xticks(range(10, 25))
ax2.set_ylim((0, 100.))
ax2.set_ylabel("(MiB/s)")
ax2.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax3 = fig.add_subplot(gs[2:,:])
ax3.bar([v-0.25 for v in avg_total_tps['pt'].keys()], avg_total_tps['pt'].values(), width=0.25, color="lightgreen", edgecolor="white", label="PT")
ax3.bar([v      for v in avg_total_tps['wa'].keys()], avg_total_tps['wa'].values(), width=0.25, color="lime",       edgecolor="white", label="WA")
ax3.bar([v+0.25 for v in avg_total_tps['mf'].keys()], avg_total_tps['mf'].values(), width=0.25, color="darkgreen",  edgecolor="white", label="MF")
ax3.set_title("Total Throughput")
ax3.set_xticks(range(10, 25))
ax3.set_ylim((0, 100.))
ax3.set_ylabel("(MiB/s)")
ax3.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax3.set_xlabel("Intensity (x1000 4K-Reqs/s)")

fig.suptitle("Throughput on Different Intensities")

plt.savefig("exp-intensity.png", dpi=120)
