-- relational_algebra.sql - the operators of relational algebra as Firebird SQL
-- (see ../../relational-algebra.md).
--
-- Every query runs read-only against the demo EMPLOYEE database, with the
-- explained plan printed above its result, so each operator can be seen next
-- to the record source that executes it:
--
--   isql -user SYSDBA -password masterkey -q inet://localhost/employee \
--        -i samples/sql/relational_algebra.sql
--
set explain on;

-- sigma: selection  (sigma dept_no='621' (EMPLOYEE))
select emp_no, last_name from employee where dept_no = '621';

-- pi: projection. SQL keeps duplicates (bag); DISTINCT is the set projection
select count(*), count(distinct job_code) from employee;
select distinct job_code from employee rows 3;

-- rho: rename
select e.emp_no as id, e.last_name as surname from employee e where e.emp_no = 2;

-- union: UNION is the set union, UNION ALL the bag union
select count(*) from (select dept_no from employee union select dept_no from department);
select count(*) from (select dept_no from employee union all select dept_no from department);

-- x: Cartesian product (21 departments x 31 jobs)
select count(*) from department cross join job;

-- natural join: the shared column DEPT_NO; USING spells the same join
select count(*) from employee natural join department;
select count(*) from employee join department using (dept_no);

-- theta join: an equality part (hashed) plus a range part (filtered)
select count(*) from employee e join job j
  on e.salary between j.min_salary and j.max_salary and e.job_country = j.job_country;

-- intersection: no INTERSECT in Firebird; a semijoin on the whole row does it
-- (employees on project VBASE and on project MAPDB)
select p.emp_no from employee_project p where p.proj_id = 'VBASE'
  and exists (select 1 from employee_project q where q.emp_no = p.emp_no and q.proj_id = 'MAPDB');

-- semijoin: EXISTS / IN become a semi join
select count(*) from department d where d.dept_no in (select e.dept_no from employee e);
select count(*) from employee e where exists (select 1 from employee_project p where p.emp_no = e.emp_no);

-- antijoin / difference: no EXCEPT; NOT EXISTS stays a correlated subquery
-- (departments with no employees = DEPARTMENT minus the departments of EMPLOYEE)
select d.dept_no, d.department from department d
 where not exists (select 1 from employee e where e.dept_no = d.dept_no);

-- NOT IN is not the antijoin: one NULL in the subquery and nothing qualifies
select count(*) from department d where d.dept_no not in (select h.head_dept from department h);
select count(*) from department d where not exists (select 1 from department h where h.head_dept = d.dept_no);

-- division: employees assigned to EVERY led software project (VBASE, MAPDB)
select e.emp_no, e.last_name from employee e
 where not exists (select 1 from project s where s.product = 'software' and s.team_leader is not null
   and not exists (select 1 from employee_project r where r.emp_no = e.emp_no and r.proj_id = s.proj_id));
-- ... and the counting form of the same division
select r.emp_no from employee_project r join project s on s.proj_id = r.proj_id
 where s.product = 'software' and s.team_leader is not null
 group by r.emp_no
 having count(*) = (select count(*) from project where product = 'software' and team_leader is not null);

-- outer joins: left, right (the mirrored left), full
select count(*) from department d left join employee e on e.dept_no = d.dept_no;
select count(*) from employee e right join department d on e.dept_no = d.dept_no;
select count(*) from employee e full join project p on p.team_leader = e.emp_no;

-- gamma: grouping and aggregation
select dept_no, count(*), avg(salary) from employee group by dept_no having count(*) > 4;

-- tau: sorting (extended algebra)
select first 3 last_name, salary from employee order by salary desc;

-- equivalence at work: the selections are pushed below the join
select count(*) from employee e join department d on d.dept_no = e.dept_no
 where d.location = 'Monterey' and e.salary > 50000;

-- beyond the algebra: transitive closure with a recursive CTE
with recursive sub (dept_no, department, lvl) as (
  select dept_no, department, 0 from department where dept_no = '600'
  union all
  select d.dept_no, d.department, s.lvl + 1 from department d join sub s on d.head_dept = s.dept_no)
select dept_no, department, lvl from sub;
