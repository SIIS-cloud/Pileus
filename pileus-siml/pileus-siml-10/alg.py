import random

filp = open('algorithm.txt','a')
for i in range(50, 2000):
    num = 25 
    filp.write('%s\t%s\n'%(i, num))
filp.close()
