import operator
import sys

# number of nodes
total_m = 10
# number of pick
total_b = 5
# number of times
total_r = 100 
# number of clients
total_n = 1000

cache = [([0]* (total_r+1)) for i in range(total_m+1)]

sys.setrecursionlimit(total_r*total_m)

def c(n,k): 
    if n == 0 or k == 0:
        return 1
    else:
        return  reduce(operator.mul, range(n - k + 1, n + 1)) / reduce(operator.mul, range(1, k +1))  

def function(m, m_p, b, r):
    if m_p <=b:
        return 1
    if r*b < m_p: 
        return 0
    x = min(b, m_p - b)
    result = 0
    for i in range(0, x):
        f = 0
        if cache[m_p-i][r-1] != 0:
            f = cache[m_p-i][r-1]
        else:
            f = function(m, m_p-i, b, r-1)
            cache[m_p-i][r-1] = f
        #a = f*c(m_p-i, b-i)*c(m-m_p+i, i)/c(m,b)
        print "[%s][%s][%s]"%(m_p, r, i)
        result = result + f*c(m_p-i, b-i)*c(m-m_p+i, i)/c(m,b)
    cache[m_p][r] = result
    #print "[%s][%s]:%s"%(m_p, r, result)
    return result

print "result is %s"%function(total_m, total_m, total_b, total_r)
