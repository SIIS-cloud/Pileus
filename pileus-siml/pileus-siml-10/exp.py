import random

filp = open('expiration.txt','a')
for i in range(101, 3001):
    rand = random.randint(1,3) 
    num = 528 + rand
    filp.write('%s\t%s\n'%(i, num))
filp.close()
