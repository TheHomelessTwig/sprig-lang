; ModuleID = 'sprig'
source_filename = "sprig"

@.fmt_num = private unnamed_addr constant [4 x i8] c"%g\0A\00", align 1
@.fmt_num.1 = private unnamed_addr constant [4 x i8] c"%g\0A\00", align 1
@.str = private unnamed_addr constant [6 x i8] c"sprig\00", align 1
@.str.2 = private unnamed_addr constant [7 x i8] c"Hello \00", align 1
@.cat_fmt = private unnamed_addr constant [5 x i8] c"%s%s\00", align 1
@.str.3 = private unnamed_addr constant [2 x i8] c"!\00", align 1
@.cat_fmt.4 = private unnamed_addr constant [5 x i8] c"%s%s\00", align 1
@.fmt_str = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.5 = private unnamed_addr constant [13 x i8] c"four is even\00", align 1
@.fmt_str.6 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.7 = private unnamed_addr constant [12 x i8] c"four is odd\00", align 1
@.fmt_str.8 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.9 = private unnamed_addr constant [2 x i8] c"A\00", align 1
@.fmt_str.10 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.11 = private unnamed_addr constant [2 x i8] c"B\00", align 1
@.fmt_str.12 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.13 = private unnamed_addr constant [2 x i8] c"C\00", align 1
@.fmt_str.14 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.str.15 = private unnamed_addr constant [12 x i8] c"counted to \00", align 1
@.tfmt = private unnamed_addr constant [3 x i8] c"%g\00", align 1
@.cat_fmt.16 = private unnamed_addr constant [5 x i8] c"%s%s\00", align 1
@.fmt_str.17 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@.fmt_num.18 = private unnamed_addr constant [4 x i8] c"%g\0A\00", align 1

declare i32 @printf(ptr, ...)

declare i32 @sprintf(ptr, ptr, ...)

declare double @atof(ptr)

define double @add(double %a, double %b) {
entry:
  %retval = alloca double, align 8
  %b2 = alloca double, align 8
  %a1 = alloca double, align 8
  store double %a, ptr %a1, align 8
  store double %b, ptr %b2, align 8
  store double 0.000000e+00, ptr %retval, align 8
  %a3 = load double, ptr %a1, align 8
  %b4 = load double, ptr %b2, align 8
  %add = fadd double %a3, %b4
  store double %add, ptr %retval, align 8
  br label %return

return:                                           ; preds = %dead, %entry
  %ret = load double, ptr %retval, align 8
  ret double %ret

dead:                                             ; No predecessors!
  br label %return
}

define double @factorial(double %n) {
entry:
  %retval = alloca double, align 8
  %n1 = alloca double, align 8
  store double %n, ptr %n1, align 8
  store double 0.000000e+00, ptr %retval, align 8
  %n2 = load double, ptr %n1, align 8
  %lt = fcmp olt double %n2, 2.000000e+00
  br i1 %lt, label %then, label %merge

return:                                           ; preds = %dead5, %merge, %then
  %ret = load double, ptr %retval, align 8
  ret double %ret

then:                                             ; preds = %entry
  store double 1.000000e+00, ptr %retval, align 8
  br label %return

merge:                                            ; preds = %dead, %entry
  %n3 = load double, ptr %n1, align 8
  %n4 = load double, ptr %n1, align 8
  %sub = fsub double %n4, 1.000000e+00
  %factorial_ret = call double @factorial(double %sub)
  %mul = fmul double %n3, %factorial_ret
  store double %mul, ptr %retval, align 8
  br label %return

dead:                                             ; No predecessors!
  br label %merge

dead5:                                            ; No predecessors!
  br label %return
}

define double @is_even(double %n) {
entry:
  %retval = alloca double, align 8
  %n1 = alloca double, align 8
  store double %n, ptr %n1, align 8
  store double 0.000000e+00, ptr %retval, align 8
  %n2 = load double, ptr %n1, align 8
  %div = fdiv double %n2, 2.000000e+00
  %mul = fmul double %div, 2.000000e+00
  %n3 = load double, ptr %n1, align 8
  %eq = fcmp oeq double %mul, %n3
  %bool_to_f64 = uitofp i1 %eq to double
  store double %bool_to_f64, ptr %retval, align 8
  br label %return

return:                                           ; preds = %dead, %entry
  %ret = load double, ptr %retval, align 8
  ret double %ret

dead:                                             ; No predecessors!
  br label %return
}

define i32 @main() {
entry:
  %n = alloca double, align 8
  %concat_buf16 = alloca [1024 x i8], align 1
  %txt_buf = alloca [64 x i8], align 1
  %counter = alloca double, align 8
  %score = alloca double, align 8
  %concat_buf3 = alloca [1024 x i8], align 1
  %concat_buf = alloca [1024 x i8], align 1
  %name = alloca ptr, align 8
  %x = alloca double, align 8
  %add_ret = call double @add(double 3.000000e+00, double 4.000000e+00)
  store double %add_ret, ptr %x, align 8
  %x1 = load double, ptr %x, align 8
  %0 = call i32 (ptr, ...) @printf(ptr @.fmt_num, double %x1)
  %factorial_ret = call double @factorial(double 5.000000e+00)
  %1 = call i32 (ptr, ...) @printf(ptr @.fmt_num.1, double %factorial_ret)
  store ptr @.str, ptr %name, align 8
  %name2 = load ptr, ptr %name, align 8
  %2 = call i32 (ptr, ptr, ...) @sprintf(ptr %concat_buf, ptr @.cat_fmt, ptr @.str.2, ptr %name2)
  %3 = call i32 (ptr, ptr, ...) @sprintf(ptr %concat_buf3, ptr @.cat_fmt.4, ptr %concat_buf, ptr @.str.3)
  %4 = call i32 (ptr, ...) @printf(ptr @.fmt_str, ptr %concat_buf3)
  %is_even_ret = call double @is_even(double 4.000000e+00)
  %to_bool = fcmp one double %is_even_ret, 0.000000e+00
  br i1 %to_bool, label %then, label %else

then:                                             ; preds = %entry
  %5 = call i32 (ptr, ...) @printf(ptr @.fmt_str.6, ptr @.str.5)
  br label %merge

merge:                                            ; preds = %else, %then
  store double 8.500000e+01, ptr %score, align 8
  %score7 = load double, ptr %score, align 8
  %gt = fcmp ogt double %score7, 9.000000e+01
  br i1 %gt, label %then4, label %else6

else:                                             ; preds = %entry
  %6 = call i32 (ptr, ...) @printf(ptr @.fmt_str.8, ptr @.str.7)
  br label %merge

then4:                                            ; preds = %merge
  %7 = call i32 (ptr, ...) @printf(ptr @.fmt_str.10, ptr @.str.9)
  br label %merge5

merge5:                                           ; preds = %merge9, %then4
  store double 0.000000e+00, ptr %counter, align 8
  br label %while_hdr

else6:                                            ; preds = %merge
  %score11 = load double, ptr %score, align 8
  %gt12 = fcmp ogt double %score11, 8.000000e+01
  br i1 %gt12, label %then8, label %else10

then8:                                            ; preds = %else6
  %8 = call i32 (ptr, ...) @printf(ptr @.fmt_str.12, ptr @.str.11)
  br label %merge9

merge9:                                           ; preds = %else10, %then8
  br label %merge5

else10:                                           ; preds = %else6
  %9 = call i32 (ptr, ...) @printf(ptr @.fmt_str.14, ptr @.str.13)
  br label %merge9

while_hdr:                                        ; preds = %while_body, %merge5
  %counter13 = load double, ptr %counter, align 8
  %lt = fcmp olt double %counter13, 5.000000e+00
  br i1 %lt, label %while_body, label %while_exit

while_body:                                       ; preds = %while_hdr
  %counter14 = load double, ptr %counter, align 8
  %add = fadd double %counter14, 1.000000e+00
  store double %add, ptr %counter, align 8
  br label %while_hdr

while_exit:                                       ; preds = %while_hdr
  %counter15 = load double, ptr %counter, align 8
  %10 = call i32 (ptr, ptr, ...) @sprintf(ptr %txt_buf, ptr @.tfmt, double %counter15)
  %11 = call i32 (ptr, ptr, ...) @sprintf(ptr %concat_buf16, ptr @.cat_fmt.16, ptr @.str.15, ptr %txt_buf)
  %12 = call i32 (ptr, ...) @printf(ptr @.fmt_str.17, ptr %concat_buf16)
  store double 0.000000e+00, ptr %n, align 8
  br label %while_hdr17

while_hdr17:                                      ; preds = %merge28, %then24, %while_exit
  %n20 = load double, ptr %n, align 8
  %lt21 = fcmp olt double %n20, 1.000000e+01
  br i1 %lt21, label %while_body18, label %while_exit19

while_body18:                                     ; preds = %while_hdr17
  %n22 = load double, ptr %n, align 8
  %add23 = fadd double %n22, 1.000000e+00
  store double %add23, ptr %n, align 8
  %n26 = load double, ptr %n, align 8
  %eq = fcmp oeq double %n26, 3.000000e+00
  br i1 %eq, label %then24, label %merge25

while_exit19:                                     ; preds = %then27, %while_hdr17
  ret i32 0

then24:                                           ; preds = %while_body18
  br label %while_hdr17

merge25:                                          ; preds = %dead, %while_body18
  %n29 = load double, ptr %n, align 8
  %eq30 = fcmp oeq double %n29, 7.000000e+00
  br i1 %eq30, label %then27, label %merge28

dead:                                             ; No predecessors!
  br label %merge25

then27:                                           ; preds = %merge25
  br label %while_exit19

merge28:                                          ; preds = %dead31, %merge25
  %n32 = load double, ptr %n, align 8
  %13 = call i32 (ptr, ...) @printf(ptr @.fmt_num.18, double %n32)
  br label %while_hdr17

dead31:                                           ; No predecessors!
  br label %merge28
}
