/**************************************************************************
			GTA PROJECT   AT division
	Copyright, 1989-93, The Regents of the University of California.
		         Los Alamos National Laboratory

 	$Id: proto.h,v 1.1.1.1 2000-04-04 03:22:32 wlupton Exp $
	DESCRIPTION: Function prototypes for state notation language parser
	ENVIRONMENT: UNIX
	HISTORY:
29apr99,wfl	Created.

***************************************************************************/

#ifndef INCLsnch
#define INCLsnch

/* Don't require parse.h to have been included */
#ifndef INCLparseh
#define Expr void
#define Var  void
#endif

/* Prototypes for external functions */
extern void Global_yyparse (void);

extern void phase2(void);

extern int  exprCount(Expr*);

extern void gen_ss_code(void);

extern void gen_tables(void);

extern void addVar(Var*);

extern void print_line_num(int,char*);

extern void traverseExprTree(Expr*,int,char*,void (*)(),void*);

#endif	/*INCLsnch*/