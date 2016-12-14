import random
from sets import Set

#nodes = 1000
#users = 400 
#ops = 10 # Includes 10
#perop = 5 # 5 nodes per operation

fp = open('result.txt', 'w')

for x in range(1, 2002, 50):
    fp.write('%s\t'%x)
    oplist = []

    for i in range(0, x):
        j = random.randint(0, 399)
        oplist.append(j)


    #print len(oplist)

    # maximum capacity
    i = 0
    num = Set()
    total = 0
    for user in oplist:
        if user not in num:
            num.add(user)
        if (i == 9):
            i = 0
            total += len(num)
            num.clear()
        else:
            i = i + 1
    #print "Total nodes: %s"%(len(oplist)/10)
    #print "Total users: %s"%total
    max_utlize = 10.0 * total/len(oplist)
    fp.write('%.1f\t'%max_utlize)

    # random pick
    total_u = 0
    total_n = 0
    limit = [0 for i in range(1000)]
    num = [Set() for i in range(1000)] 
    samplist = range(0, 1000)
    for user in oplist:
        if len(samplist) < 5:
            sam = samplist
        else:
            sam = random.sample(samplist, 5)
        for j in sam:
            limit[j] = limit[j] + 1
            if limit[j] == 10:
                samplist.remove(j) 
            if user not in num[j]:
                num[j].add(user)

    for n in num:
        if len(n) != 0:
            total_n += 1
            total_u += len(n)
    #print "Total users:%s"%total_u
    #print "Total nodes:%s"%total_n
    random_pick = 1.0*total_u / total_n
    fp.write('%.1f\t'%random_pick)

    # maximum performance
    total_u = 0
    total_n = 0
    index = 0
    num = [Set() for i in range(1000)]
    for user in oplist:
        for i in range(0,5):
            if user not in num[index]:
                num[index].add(user)
            index = index + 1
            if index == 1000:
                index = 0

    for n in num:
        if len(n) != 0:
            total_u += len(n)
            total_n += 1
    #print "Total users:%s"%total_u
    #print "Total nodes:%s"%total_n
    max_perf = 1.0*total_u / total_n
    fp.write('%.1f\t'%max_perf)

    # least TCB
    total_u = 0
    total_n = 0
    users = {}
    nodes = {}
    num = [Set() for i in range(1000)]

    for i in range(0, 1000):
        nodes['%s'%i] = 0

    for i in range(0, 400):
        users['%s'%i] = []

    def add_nodes(count, user):
        import operator
        sorted_x = sorted(nodes.items(), key=operator.itemgetter(1))
        for i in range(0, count):
            nodes[sorted_x[i][0]] += 1
            tcb = users.get('%s'%user)
            tcb.append(sorted_x[i][0])
            if user not in num[int(sorted_x[i][0])]:
                num[int(sorted_x[i][0])].add(user)

    for user in oplist:
        if users.has_key('%s'%user):
            tcb = users.get('%s'%user)
            count = 0
            for j in tcb:
                if nodes[j] != 10:
                    nodes[j] += 1
                    count += 1
                    if count == 5:
                        break
            if count < 5:
                add_nodes(5 - count, user)
        else:
            add_nodes(5, user) 
            
    for n in num:
        if len(n) != 0:
            total_u += len(n)
            total_n += 1
    #print "Total users:%s"%total_u
    #print "Total nodes:%s"%total_n
    least_tcb = 1.0*total_u / total_n
    fp.write('%.1f\n'%least_tcb)
    #print len(num)
    #print num

fp.close()
