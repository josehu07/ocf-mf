import matplotlib
matplotlib.use('Agg')

import numpy as np
import matplotlib.pyplot as plt
import os
from collections import OrderedDict


resfiles = []
for filename in os.listdir("./"):
    if filename.startswith("result-") and filename.endswith(".txt"):
        resfiles.append(filename)


dev_configs = {
    "cache50-core10": {
        6400: 0.5,
        12800: 1.0,
        19200: 1.5,
        25600: 2.0,
    },
    "cache50-core25": {
        6400: 0.5,
        12800: 1.0,
        19200: 1.5,
        25600: 2.0,
    },
    "optane-flash":   {},
    "nvdimm-optane":  {},
}

for hr in (99, 80):

    avg_total_tps = {}

    for filename in resfiles:
        dev_config = filename[filename.find("-cache")+1:filename.find("-int")]
        intensity = int(filename[filename.find("-int")+4:filename.find("-read")])
        read_percentage = int(filename[filename.find("-read")+5:filename.find("-hit")])
        hit_ratio = int(filename[filename.find("-hit")+4:filename.rfind("-")])
        mode = filename[filename.rfind("-")+1:filename.find(".try")]
        try_no = int(filename[filename.find(".try")+4:filename.find(".txt")])

        if read_percentage != 100 or hit_ratio != hr \
           or dev_config not in dev_configs or intensity not in dev_configs[dev_config]:
            continue

        intensity_ratio = dev_configs[dev_config][intensity]

        if dev_config not in avg_total_tps:
            avg_total_tps[dev_config] = {}

        if mode not in avg_total_tps[dev_config]:
            avg_total_tps[dev_config][mode] = {}

        if intensity_ratio not in avg_total_tps[dev_config][mode]:
            avg_total_tps[dev_config][mode][intensity_ratio] = []


        num_reqs = []
        times = []
        cache_tps = []
        core_tps = []

        with open(filename) as resfile:
            for line in resfile.readlines():
                line = line.strip()

                if line.startswith("..."):
                    num_req = int(line[line.find("#")+1:line.find(" @")])
                    time = float(line[line.find("@ ")+2:line.find(" ms")])
                    cache_tp = float(line[line.find("cache_tp = ")+11:line.find(", core")])
                    core_tp = float(line[line.find("core_tp = ")+10:])

                    num_reqs.append(num_req)
                    times.append(time)
                    cache_tps.append(cache_tp / 1024.0)     # MiB/s
                    core_tps.append(core_tp / 1024.0)       # MiB/s

        # Amplification ratio: actual number of requests may be smaller than what it
        # should be given the intensity number, because there is overhead in benchmarking
        # code loop. Doing usleep of delta (= 1 / intensity) will actually give an
        # intensity smaller than what we set.
        time_length = (times[-1] - times[0]) / 1000
        amp_ratio = float(intensity * time_length) / float(num_reqs[-1])

        avg_cache_tp = 0
        avg_core_tp = 0

        for i in range(len(times)):
            avg_cache_tp += cache_tps[i] * amp_ratio
            avg_core_tp += core_tps[i] * amp_ratio

        cache_bandwidth = None
        for target_int in dev_configs[dev_config]:
            if dev_configs[dev_config][target_int] == 1.0:
                cache_bandwidth = (target_int * 4) / 1024.
                break

        avg_total_tps[dev_config][mode][intensity_ratio].append(((avg_cache_tp + avg_core_tp) / len(times))
                                                                / cache_bandwidth)

    for dev_config in avg_total_tps.keys():
        for mode in avg_total_tps[dev_config].keys():
            for intensity_ratio in avg_total_tps[dev_config][mode].keys():
                avg_total_tps[dev_config][mode][intensity_ratio] = np.mean(avg_total_tps[dev_config][mode][intensity_ratio])
            avg_total_tps[dev_config][mode] = OrderedDict(sorted(avg_total_tps[dev_config][mode].items()))

    # print(avg_total_tps)

    if len(avg_total_tps) == 0:
        continue

    throughput_ylim = (0., 2.)
    xticks_range = np.arange(0.5, 2.5, 0.5)
    bar_width = 0.2
    bar_shift = bar_width / 2

    fig = plt.figure(filename, figsize=(12, 4))

    ax1 = fig.add_subplot(141)
    if "cache50-core10" in avg_total_tps:
        ax1.bar([v-bar_shift for v in avg_total_tps["cache50-core10"]['wa'].keys()],
                avg_total_tps["cache50-core10"]['wa'].values(),
                width=bar_width, color="grey", edgecolor="white", hatch='/', label="classic")
        ax1.bar([v+bar_shift for v in avg_total_tps["cache50-core10"]['mfwa'].keys()],
                avg_total_tps["cache50-core10"]['mfwa'].values(),
                width=bar_width, color="black", edgecolor="white", label="multi-factor")
    ax1.set_xlabel("Intensity : Cache bandwidth\n\nSimulated 50 + 10")
    ax1.set_xticks(xticks_range)
    ax1.set_ylim(throughput_ylim)
    ax1.set_ylabel("Total throughput : Cache bandwidth")

    ax2 = fig.add_subplot(142)
    if "cache50-core25" in avg_total_tps:
        ax2.bar([v-bar_shift for v in avg_total_tps["cache50-core25"]['wa'].keys()],
                avg_total_tps["cache50-core25"]['wa'].values(),
                width=bar_width, color="grey", edgecolor="white", hatch='/', label="classic")
        ax2.bar([v+bar_shift for v in avg_total_tps["cache50-core25"]['mfwa'].keys()],
                avg_total_tps["cache50-core25"]['mfwa'].values(),
                width=bar_width, color="black", edgecolor="white", label="multi-factor")
    ax2.set_xlabel("Intensity : Cache bandwidth\n\nSimulated 50 + 25")
    ax2.set_xticks(xticks_range)
    ax2.set_ylim(throughput_ylim)

    ax3 = fig.add_subplot(143)
    if "optane-flash" in avg_total_tps:
        ax3.bar([v-bar_shift for v in avg_total_tps["optane-flash"]['wa'].keys()],
                avg_total_tps["optane-flash"]['wa'].values(),
                width=bar_width, color="grey", edgecolor="white", hatch='/', label="classic")
        ax3.bar([v+bar_shift for v in avg_total_tps["optane-flash"]['mfwa'].keys()],
                avg_total_tps["optane-flash"]['mfwa'].values(),
                width=bar_width, color="black", edgecolor="white", label="multi-factor")
    ax3.set_xlabel("Intensity : Cache bandwidth\n\nOptane + Flash")
    ax3.set_xticks(xticks_range)
    ax3.set_ylim(throughput_ylim)

    ax4 = fig.add_subplot(144)
    if "nvdimm-optane" in avg_total_tps:
        ax4.bar([v-bar_shift for v in avg_total_tps["nvdimm-optane"]['wa'].keys()],
                avg_total_tps["nvdimm-optane"]['wa'].values(),
                width=bar_width, color="grey", edgecolor="white", hatch='/', label="classic")
        ax4.bar([v+bar_shift for v in avg_total_tps["nvdimm-optane"]['mfwa'].keys()],
                avg_total_tps["nvdimm-optane"]['mfwa'].values(),
                width=bar_width, color="black", edgecolor="white", label="multi-factor")
    ax4.set_xlabel("Intensity : Cache bandwidth\n\nNVDIMM + Optane")
    ax4.set_xticks(xticks_range)
    ax4.set_ylim(throughput_ylim)
    ax4.legend(bbox_to_anchor=(1.02, 1), loc="upper left")

    fig.tight_layout(pad=1.0, rect=[0, 0.02, 1, 0.9])

    fig.suptitle("Figure 1. MFC Improvement, Hit ratio ~= "+str(hr)+"%")

    plt.savefig("figure1-hit"+str(hr)+".png", dpi=200)
    plt.close()
