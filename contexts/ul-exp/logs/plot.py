import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt


cache_proc_times = {}
cache_queue_depths = {}

core_proc_times = {}
core_queue_depths = {}

with open("device-log.txt") as flog:
    for line in flog.readlines():
        line = line.strip()

        if line.startswith("cache req"):
            start_time = float(line[line.find(": ")+2:line.find(" - ")])
            finish_time = float(line[line.find("- ")+2:line.find(" of")])
            cache_proc_times[start_time] = finish_time - start_time

        elif line.startswith("core req"):
            start_time = float(line[line.find(": ")+2:line.find(" - ")])
            finish_time = float(line[line.find("- ")+2:line.find(" of")])
            core_proc_times[start_time] = finish_time - start_time

        elif line.startswith("cache queue"):
            start_time = float(line[line.find("@ ")+2:line.find(", ")])
            queue_depth = int(line[line.find("= ")+2:])
            cache_queue_depths[start_time] = queue_depth

        elif line.startswith("core queue"):
            start_time = float(line[line.find("@ ")+2:line.find(", ")])
            queue_depth = int(line[line.find("= ")+2:])
            core_queue_depths[start_time] = queue_depth

    # print(cache_proc_times)
    # print(cache_queue_depths)

    # print(core_proc_times)
    # print(core_queue_dpeths)

    fig = plt.figure("device-log")

    plt.subplot(4, 1, 1)
    xs, ys = zip(*sorted(cache_proc_times.items()))
    plt.scatter(xs, ys, marker='.', s=5, c='c')
    plt.ylabel("(ms)")
    plt.title("Cache Request Queueing + Processing Time")

    plt.subplot(4, 1, 2)
    xs1, ys1 = zip(*[tup for tup in sorted(cache_queue_depths.items()) if tup[1] > 1])
    xs2, ys2 = zip(*[tup for tup in sorted(cache_queue_depths.items()) if tup[1] <= 1])
    plt.scatter(xs1, ys1, marker='.', s=5, c='b', label="> 1")
    plt.scatter(xs2, ys2, marker='.', s=5, c='r', label="Empty")
    plt.ylabel("Length")
    plt.legend()
    plt.title("Cache Queue Depth")

    plt.subplot(4, 1, 3)
    xs, ys = zip(*sorted(core_proc_times.items()))
    plt.scatter(xs, ys, marker='.', s=5, c='c')
    plt.ylabel("(ms)")
    plt.title("Core Request Queueing + Processing Time")

    plt.subplot(4, 1, 4)
    xs1, ys1 = zip(*[tup for tup in sorted(core_queue_depths.items()) if tup[1] > 1])
    xs2, ys2 = zip(*[tup for tup in sorted(core_queue_depths.items()) if tup[1] <= 1])
    plt.scatter(xs1, ys1, marker='.', s=5, c='b', label="> 1")
    plt.scatter(xs2, ys2, marker='.', s=5, c='r', label="Empty")
    plt.ylabel("Length")
    plt.legend()
    plt.title("Core Queue Depth")

    plt.xlabel("Reqeust start time (ms)")

    fig.tight_layout(pad=1.0)

    plt.savefig("device-log.png")
