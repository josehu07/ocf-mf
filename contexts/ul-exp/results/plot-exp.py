import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt
import os
from collections import OrderedDict


resfiles = []
for filename in os.listdir("./"):
    if filename.startswith("result-") and filename.endswith(".txt"):
        resfiles.append(filename)


avg_load_admits = {'mf': {}, 'wa': {}}
avg_cache_tps = {'mf': {}, 'wa': {}}
avg_core_tps = {'mf': {}, 'wa': {}}
avg_total_tps = {'mf': {}, 'wa': {}}

for filename in resfiles:
    intensity = int(filename[filename.find("-")+1:filename.rfind("-")])
    mode = filename[filename.rfind("-")+1:filename.find(".txt")]

    # num_reqs = []
    times = []
    # miss_ratios = []
    load_admits = []
    cache_tps = []
    core_tps = []

    with open(filename) as resfile:
        for line in resfile.readlines():
            line = line.strip()

            if line.startswith("..."):
                # num_req = int(line[line.find("#")+1:line.find(" @")])
                time = float(line[line.find("@ ")+2:line.find(" ms")])
                # miss_ratio = float(line[line.find("miss_ratio = ")+13:line.find(", load_admit")])
                load_admit = float(line[line.find("load_admit = ")+13:line.find(", cache")])
                cache_tp = float(line[line.find("cache_tp = ")+11:line.find(", core")])
                core_tp = float(line[line.find("core_tp = ")+10:])

                # num_reqs.append(num_req)
                times.append(time)
                # miss_ratios.append(miss_ratio)
                load_admits.append(load_admit)
                cache_tps.append(cache_tp)
                core_tps.append(core_tp)

    idx_begin, idx_end = None, None

    for i in range(len(times)):
        if idx_begin is None and times[i] > 50000:
            idx_begin = i
        if idx_end is None and times[i] >= 150000:
            idx_end = i

    # print(idx_begin, idx_end, len(times))

    avg_load_admit = 0
    avg_cache_tp = 0
    avg_core_tp = 0

    for i in range(idx_begin, idx_end):
        avg_load_admit += load_admits[i]
        avg_cache_tp += cache_tps[i]
        avg_core_tp += core_tps[i]

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
ax1.bar([v-40 for v in avg_cache_tps['wa'].keys()], avg_cache_tps['wa'].values(), width=80, color="lightsteelblue", edgecolor="white", label="WA")
ax1.bar([v+40 for v in avg_cache_tps['mf'].keys()], avg_cache_tps['mf'].values(), width=80, color="blue",           edgecolor="white", label="MF")
ax1.set_title("Cache Throughput")
ax1.set_xticks(range(2000, 4200, 200))
ax1.set_ylim((0, 16000))
ax1.set_ylabel("(KB/s)")
ax1.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax2 = fig.add_subplot(gs[1,:])
ax2.bar([v-40 for v in avg_core_tps['wa'].keys()], avg_core_tps['wa'].values(), width=80, color="bisque",     edgecolor="white", label="WA")
ax2.bar([v+40 for v in avg_core_tps['mf'].keys()], avg_core_tps['mf'].values(), width=80, color="darkorange", edgecolor="white", label="MF")
ax2.set_title("Core Throughput")
ax2.set_xticks(range(2000, 4200, 200))
ax2.set_ylim((0, 16000))
ax2.set_ylabel("(KB/s)")
ax2.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax3 = fig.add_subplot(gs[2:,:])
ax3.bar([v-40 for v in avg_total_tps['wa'].keys()], avg_total_tps['wa'].values(), width=80, color="lightgreen", edgecolor="white", label="WA")
ax3.bar([v+40 for v in avg_total_tps['mf'].keys()], avg_total_tps['mf'].values(), width=80, color="green",      edgecolor="white", label="MF")
ax3.set_title("Total Throughput")
ax3.set_xticks(range(2000, 4200, 200))
ax3.set_ylim((0, 16000))
ax3.set_ylabel("(KB/s)")
ax3.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

ax3.set_xlabel("Intensity (#4K-Reqs/s)")

fig.suptitle("Throughput on Different Intensities")

plt.savefig("exp-intensity.png", dpi=200)
