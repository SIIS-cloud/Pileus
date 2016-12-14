import random

filp = open('expiration.txt','a')
for i in range(16, 2001):
    rand = random.randint(1,3) 
    num = 72.4
    filp.write('%s\t%s\n'%(i, num))
filp.close()
