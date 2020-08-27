import os


measurefiles = []
for filename in os.listdir("./"):
    if filename.startswith("measure-") and filename.endswith(".txt"):
        measurefiles.append(filename)


for filename in measurefiles:
    direction = filename[filename.find("measure-")+8:filename.find("-int")]
    intensity = int(filename[filename.find("-int")+4:filename.find(".txt")])

    num_reqs = []
    times = []
    cache_tps = []

    with open(filename) as resfile:
        for line in resfile.readlines():
            line = line.strip()

            if line.startswith("..."):
                num_req = int(line[line.find("#")+1:line.find(" @")])
                time = float(line[line.find("@ ")+2:line.find(" ms")])
                cache_tp = float(line[line.find("cache_tp = ")+11:line.find(", core")])

                num_reqs.append(num_req)
                times.append(time)
                cache_tps.append(cache_tp / 1024.0)     # MiB/s

    # Amplification ratio: actual number of requests may be smaller than what it
    # should be given the intensity number, because there is overhead in benchmarking
    # code loop. Doing usleep of delta (= 1 / intensity) will actually give an
    # intensity smaller than what we set.
    amp_ratio = float(intensity * (160 - 60)) / float(num_reqs[-1])

    avg_cache_tp = 0

    for i in range(len(times)):
        avg_cache_tp += cache_tps[i] * amp_ratio

    avg_cache_tp /= len(times)

    print(direction+": {:.3f} MiB/s".format(avg_cache_tp))
