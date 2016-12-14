import random
from sets import Set

#nodes = 1000
#users = 400 
#ops = 10 # Includes 10
#perop = 5 # 5 nodes per operation
# step = 50 # 50 nodes should be filled before we consider new nodes

steps = 1000 # minimum is 5, maximum is 1000 

fp = open('result_balance_%s.txt'%steps, 'w')

for x in range(50, 2001, 50):
    fp.write('%s\t'%x)
    oplist = []

    for i in range(0, x):
        j = random.randint(0, 399)
        oplist.append(j)


    #print len(oplist)

    # least TCB
    total_u = 0
    total_n = 0
    users = {}
    nodes = {} # nodes['1'] = number of total ops
    nodes_u = {} # nodes_u['1'] =  number of distinct users
    temp_nodes = {}
    temp_nodes_u = {}
    num = [Set() for i in range(1000)] # set of distinct users
    counter = 0

    #for i in range(0, 1000):
    #    nodes['%s'%i] = 0

    for i in range(0, 400):
        users['%s'%i] = []

    def initiate(current):
        temp_nodes.clear()
        temp_nodes_u.clear()
        for i in range(0, 400):
            users['%s'%i] = []
        for i in range(current, current + steps):
            temp_nodes['%s'%i] = 0
            temp_nodes_u['%s'%i] = 0

    def add_nodes(count, user, counter):
        import operator
        sorted_x = sorted(temp_nodes_u.items(), key=operator.itemgetter(1))
        i = 0
        while count != 0:
            if i == steps:
                initiate(counter / 2)
                sorted_x = sorted(temp_nodes_u.items(), key=operator.itemgetter(1))
                i = 0
            #print 'index in group:%s'%i
            if temp_nodes[sorted_x[i][0]] >= 10:
                i += 1
                continue
            temp_nodes[sorted_x[i][0]] += 1
            tcb = users.get('%s'%user)
            tcb.append(sorted_x[i][0])
            #print 'index of group:%s'%i
            if user not in num[int(sorted_x[i][0])]:
                #print 'node:%s'%int(sorted_x[i][0])
                #print 'counter:%s'%counter
                num[int(sorted_x[i][0])].add(user)
                temp_nodes_u[sorted_x[i][0]] = len(num[int(sorted_x[i][0])])
            i += 1
            count -= 1

    initiate(0)
    for user in oplist:
        if users.has_key('%s'%user):
            tcb = users.get('%s'%user)
            count = 0
            for j in tcb:
                if temp_nodes[j] != 10:
                    temp_nodes[j] += 1
                    count += 1
                    if count == 5:
                        break
            if count < 5:
                add_nodes(5 - count, user, counter)
        else:
            add_nodes(5, user, counter)
        counter += 1
            
    for n in num:
        if len(n) != 0:
            total_u += len(n)
            total_n += 1
            #print len(n)
    #print "Total users:%s"%total_u
    #print "Total nodes:%s"%total_n
    least_tcb = 1.0*total_u / total_n
    fp.write('%.1f\n'%least_tcb)
    #print len(num)

fp.close()
