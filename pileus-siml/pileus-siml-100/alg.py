import random

num = 0
filp = open('algorithm.txt','a')
for i in range(0, 20):
    num = num + 5
    for j in range(1, 11):
        filp.write('%s\t%s\n'%(i*10+j, num))
for i in range(20, 80):
    for j in range(1, 11):
        filp.write('%s\t%s\n'%(i*10+j, 100+(i*10+j-200)*0.25))


for i in range(80, 200):
    for j in range(1, 11):
        filp.write('%s\t%s\n'%(i*10+j, 250))
filp.close()
